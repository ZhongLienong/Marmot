use super::manifest::{self, platform_prebuilt_key};
use super::version::{Constraint, Version};
use crate::checksum;
use serde_json::Value;
use std::collections::BTreeMap;
use std::fs::File;
use std::io;
use std::path::{Path, PathBuf};
use ureq::tls::{RootCerts, TlsConfig};
use ureq::typestate::WithoutBody;
use ureq::{Agent, RequestBuilder};

pub(crate) const DEFAULT_REGISTRY: &str = "https://github.com/ZhongLienong/marmot-packages";

/// Which packages resolution looks up in the registries.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Lookup {
    /// Every package, so a newer release wins over a copy already on disk.
    Always,
    /// Only packages no copy on disk satisfies, so a project whose packages
    /// are present resolves without the network.
    WhenMissing,
}

/// GitHub repositories whose releases are packages. A release is tagged
/// `Name-vX.Y.Z` and carries `Name-X.Y.Z.tar.gz`, the package's files, and,
/// for a package with native code, `Name-X.Y.Z-<platform>.tar.gz`, its built
/// library at the path the manifest expects.
pub(crate) struct Registries {
    repositories: Vec<String>,
    cache: PathBuf,
    lookup: Lookup,
    http: Agent,
    /// Released versions per package name, newest first, each with the index
    /// of the repository that released it.
    released: BTreeMap<String, Vec<(Version, usize)>>,
}

/// `owner/repo` from a `https://github.com/owner/repo` URL.
fn repository(url: &str) -> Result<String, String> {
    let path = url
        .strip_prefix("https://github.com/")
        .ok_or_else(|| format!("Registry '{url}' is not a https://github.com/ repository."))?;
    let path = path.trim_end_matches('/');
    let path = path.strip_suffix(".git").unwrap_or(path);
    let parts: Vec<&str> = path.split('/').collect();
    if parts.len() != 2 || parts.iter().any(|part| part.is_empty()) {
        return Err(format!(
            "Registry '{url}' is not a https://github.com/owner/repo URL."
        ));
    }
    Ok(path.to_string())
}

/// The version a `refs/tags/Name-vX.Y.Z` tag releases; other tags that share
/// the prefix, such as `Name-vnext`, release nothing.
fn tagged_version(reference: &str, name: &str) -> Option<Version> {
    reference
        .strip_prefix("refs/tags/")?
        .strip_prefix(name)?
        .strip_prefix("-v")
        .and_then(|text| Version::parse(text).ok())
}

fn registry_request(http: &Agent, url: &str) -> RequestBuilder<WithoutBody> {
    let request = http.get(url);
    if (url.starts_with("https://api.github.com/") || url.starts_with("https://github.com/"))
        && let Ok(token) = std::env::var("GITHUB_TOKEN")
        && !token.is_empty()
    {
        return request.header("Authorization", format!("Bearer {token}"));
    }
    request
}

fn github_api(http: &Agent, path: &str) -> Result<Value, String> {
    let url = format!("https://api.github.com/{path}");
    let response = registry_request(http, &url)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28")
        .call()
        .map_err(|error| format!("GitHub request for {path} failed: {error}"))?;
    serde_json::from_reader(response.into_body().into_reader())
        .map_err(|error| format!("GitHub returned unreadable JSON for {path}: {error}"))
}

/// Downloads a release asset to `target` and checks it against the sha256
/// digest GitHub recorded when it was uploaded.
fn download(http: &Agent, asset: &Value, target: &Path) -> Result<(), String> {
    let field = |key: &str| {
        asset
            .get(key)
            .and_then(Value::as_str)
            .ok_or_else(|| format!("A release asset has no '{key}'."))
    };
    let (name, url, digest) = (
        field("name")?,
        field("browser_download_url")?,
        field("digest")?,
    );
    let response = registry_request(http, url)
        .call()
        .map_err(|error| format!("Failed to download release asset '{name}': {error}"))?;
    {
        let mut archive = File::create(target)
            .map_err(|error| format!("Failed to create '{}': {error}", target.display()))?;
        io::copy(&mut response.into_body().into_reader(), &mut archive)
            .map_err(|error| format!("Failed to write release asset '{name}': {error}"))?;
    }
    let actual = checksum::file(target)?;
    if actual != digest {
        return Err(format!(
            "Release asset '{name}' has checksum {actual}, but GitHub recorded {digest}."
        ));
    }
    Ok(())
}

fn unpack(archive: &Path, directory: &Path) -> Result<(), String> {
    let file = std::fs::File::open(archive)
        .map_err(|error| format!("Failed to open '{}': {error}", archive.display()))?;
    tar::Archive::new(flate2::read::GzDecoder::new(file))
        .unpack(directory)
        .map_err(|error| format!("Failed to unpack '{}': {error}", archive.display()))
}

fn asset<'a>(release: &'a Value, name: &str) -> Option<&'a Value> {
    release
        .get("assets")
        .and_then(Value::as_array)?
        .iter()
        .find(|asset| asset.get("name").and_then(Value::as_str) == Some(name))
}

impl Registries {
    pub(crate) fn new(urls: &[String], cache: PathBuf, lookup: Lookup) -> Result<Self, String> {
        Ok(Registries {
            repositories: urls
                .iter()
                .map(|url| repository(url))
                .collect::<Result<_, _>>()?,
            cache,
            lookup,
            http: Agent::config_builder()
                .https_only(true)
                .user_agent(concat!("marmot/", env!("CARGO_PKG_VERSION")))
                .tls_config(
                    TlsConfig::builder()
                        .root_certs(RootCerts::PlatformVerifier)
                        .build(),
                )
                .build()
                .into(),
            released: BTreeMap::new(),
        })
    }

    pub(crate) fn lookup(&self) -> Lookup {
        self.lookup
    }

    fn releases(&mut self, name: &str) -> Result<&[(Version, usize)], String> {
        if !self.released.contains_key(name) {
            let mut versions = Vec::new();
            for (priority, repository) in self.repositories.iter().enumerate() {
                let references = github_api(
                    &self.http,
                    &format!("repos/{repository}/git/matching-refs/tags/{name}-v"),
                )?;
                versions.extend(
                    references
                        .as_array()
                        .map(Vec::as_slice)
                        .unwrap_or_default()
                        .iter()
                        .filter_map(|reference| reference.get("ref").and_then(Value::as_str))
                        .filter_map(|reference| tagged_version(reference, name))
                        .map(|version| (version, priority)),
                );
            }
            versions.sort_by(|(left, left_priority), (right, right_priority)| {
                right.cmp(left).then(left_priority.cmp(right_priority))
            });
            self.released.insert(name.to_string(), versions);
        }
        Ok(&self.released[name])
    }

    /// The newest released version of `name`, among those `constraint` allows
    /// when there is one.
    pub(crate) fn newest(
        &mut self,
        name: &str,
        constraint: Option<&Constraint>,
    ) -> Result<Option<Version>, String> {
        Ok(self
            .releases(name)?
            .iter()
            .map(|(version, _)| version)
            .find(|version| constraint.is_none_or(|constraint| constraint.matches(version)))
            .cloned())
    }

    /// Downloads a released version into the global cache, where the package
    /// index finds it, and returns its directory.
    pub(crate) fn fetch(
        &mut self,
        name: &str,
        version: &Version,
        compiler: &Version,
    ) -> Result<PathBuf, String> {
        let target = self.cache.join(format!("{name}-{version}"));
        if target.exists() {
            return Ok(target);
        }
        let priority = self
            .releases(name)?
            .iter()
            .find(|(released, _)| released == version)
            .map(|(_, priority)| *priority)
            .ok_or_else(|| format!("No registry releases {name} {version}."))?;
        let repository = &self.repositories[priority];
        let tag = format!("{name}-v{version}");
        eprintln!("Downloading {name} {version} from github.com/{repository}");
        let release = github_api(
            &self.http,
            &format!("repos/{repository}/releases/tags/{tag}"),
        )?;

        // Unpacked one level below the cache root, a partial download is
        // never mistaken for a package by the index.
        let partial = self.cache.join(".partial");
        let staging = partial.join(format!("{name}-{version}"));
        if staging.exists() {
            std::fs::remove_dir_all(&staging)
                .map_err(|error| format!("Failed to clear '{}': {error}", staging.display()))?;
        }
        std::fs::create_dir_all(&staging)
            .map_err(|error| format!("Failed to create '{}': {error}", staging.display()))?;

        let install = |asset_name: &str| -> Result<(), String> {
            let archive = partial.join(asset_name);
            download(
                &self.http,
                asset(&release, asset_name).ok_or_else(|| {
                    format!("Release {tag} in github.com/{repository} has no asset '{asset_name}'.")
                })?,
                &archive,
            )?;
            unpack(&archive, &staging)?;
            std::fs::remove_file(&archive)
                .map_err(|error| format!("Failed to remove '{}': {error}", archive.display()))
        };

        install(&format!("{name}-{version}.tar.gz"))?;
        let package = manifest::read_package(&staging, compiler)?;
        if package.name != name || package.version != *version {
            return Err(format!(
                "Release {tag} in github.com/{repository} holds {} {}.",
                package.name, package.version_text
            ));
        }
        if package.native.is_some() {
            install(&format!(
                "{name}-{version}-{}.tar.gz",
                platform_prebuilt_key()
            ))?;
        }

        std::fs::rename(&staging, &target).map_err(|error| {
            format!(
                "Failed to move '{}' to '{}': {error}",
                staging.display(),
                target.display()
            )
        })?;
        Ok(target)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader, Write};
    use std::net::TcpListener;
    use std::time::Duration;

    #[test]
    fn registries_are_github_repositories() {
        for url in [
            "https://github.com/ZhongLienong/marmot-packages",
            "https://github.com/ZhongLienong/marmot-packages/",
            "https://github.com/ZhongLienong/marmot-packages.git",
        ] {
            assert_eq!(repository(url).unwrap(), "ZhongLienong/marmot-packages");
        }
        for url in [
            "http://github.com/a/b",
            "https://gitlab.com/a/b",
            "https://github.com/a",
            "https://github.com/a/b/c",
        ] {
            assert!(repository(url).is_err(), "{url}");
        }
    }

    #[test]
    fn only_version_tags_of_the_package_release_it() {
        let version = |reference: &str| tagged_version(reference, "Image").map(|v| v.to_string());
        assert_eq!(version("refs/tags/Image-v0.1.0").as_deref(), Some("0.1.0"));
        assert_eq!(
            version("refs/tags/Image-v1.0.0-beta.1").as_deref(),
            Some("1.0.0-beta.1")
        );
        assert_eq!(version("refs/tags/Image-vnext"), None);
        assert_eq!(version("refs/tags/ImageTools-v1.0.0"), None);
        assert_eq!(version("refs/heads/Image-v1.0.0"), None);
    }

    #[test]
    fn a_release_download_follows_redirects_and_preserves_archive_bytes() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("http://{}/download", listener.local_addr().unwrap());
        let contents = b"archive\0\xff\r\n";
        let server = std::thread::spawn(move || {
            let redirect = b"HTTP/1.1 302 Found\r\nLocation: /asset\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                contents.len()
            );
            for (headers, body) in [
                (redirect.as_slice(), &[][..]),
                (response.as_bytes(), contents.as_slice()),
            ] {
                let (mut stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(5)))
                    .unwrap();
                let mut reader = BufReader::new(&mut stream);
                let mut line = String::new();
                loop {
                    assert!(reader.read_line(&mut line).unwrap() > 0);
                    if line == "\r\n" {
                        break;
                    }
                    line.clear();
                }
                stream.write_all(headers).unwrap();
                stream.write_all(body).unwrap();
            }
        });
        let tree = crate::test_support::TempTree::new(&[]);
        let expected = tree.path("expected");
        std::fs::write(&expected, contents).unwrap();
        let asset = serde_json::json!({
            "name": "Lib-1.0.0.tar.gz",
            "browser_download_url": url,
            "digest": checksum::file(&expected).unwrap(),
        });
        let http: Agent = Agent::config_builder()
            .proxy(None)
            .timeout_global(Some(Duration::from_secs(5)))
            .build()
            .into();
        let archive = tree.path("Lib-1.0.0.tar.gz");
        download(&http, &asset, &archive).unwrap();
        assert_eq!(std::fs::read(&archive).unwrap(), contents);
        server.join().unwrap();
    }

    #[test]
    fn a_release_archive_unpacks_into_the_package_directory() {
        let tree = crate::test_support::TempTree::new(&[]);
        let archive = tree.path("Lib-1.0.0.tar.gz");
        let mut builder = tar::Builder::new(flate2::write::GzEncoder::new(
            std::fs::File::create(&archive).unwrap(),
            flate2::Compression::default(),
        ));
        let contents = b"module Lib\n";
        let mut header = tar::Header::new_gnu();
        header.set_size(contents.len() as u64);
        header.set_mode(0o644);
        header.set_cksum();
        builder
            .append_data(&mut header, "Lib/Inner.mmt", &contents[..])
            .unwrap();
        builder.into_inner().unwrap().finish().unwrap();

        unpack(&archive, &tree.path("out")).unwrap();
        assert_eq!(tree.read("out/Lib/Inner.mmt"), "module Lib\n");
    }
}
