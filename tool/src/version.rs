use std::cmp::Ordering;
use std::fmt;

/// A semantic version. Build metadata is kept for display but ignored when
/// comparing, as SemVer requires.
#[derive(Debug, Clone, Default)]
pub struct Version {
    pub major: u64,
    pub minor: u64,
    pub patch: u64,
    pub prerelease: Vec<String>,
    pub build: Vec<String>,
}

fn identifiers(text: &str, label: &str) -> Result<Vec<String>, String> {
    text.split('.')
        .map(|identifier| {
            if identifier.is_empty() {
                Err(format!("Invalid empty {label} identifier."))
            } else if !identifier
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '-')
            {
                Err(format!("Invalid {label} identifier '{identifier}'."))
            } else {
                Ok(identifier.to_string())
            }
        })
        .collect()
}

fn component(text: &str, label: &str) -> Result<u64, String> {
    if text.is_empty() {
        return Err(format!("Missing {label} component."));
    }
    if !text.bytes().all(|b| b.is_ascii_digit()) {
        return Err(format!("Invalid {label} component '{text}'."));
    }
    // The compiler stores components as int.
    text.parse::<i32>()
        .map(|value| value as u64)
        .map_err(|_| format!("Invalid {label} component '{text}'."))
}

impl Version {
    pub fn parse(text: &str) -> Result<Version, String> {
        let trimmed = text.trim();
        if trimmed.is_empty() {
            return Err("Version string is empty.".to_string());
        }

        let (without_build, build) = match trimmed.split_once('+') {
            Some((head, build)) => (head, Some(build)),
            None => (trimmed, None),
        };
        let (core, prerelease) = match without_build.split_once('-') {
            Some((core, prerelease)) => (core, Some(prerelease)),
            None => (without_build, None),
        };

        let mut parts = core.splitn(3, '.');
        let major = parts.next().unwrap_or("");
        let minor = parts.next().ok_or_else(|| {
            format!("Version '{trimmed}' is missing the minor or patch component.")
        })?;
        let patch = parts
            .next()
            .ok_or_else(|| format!("Version '{trimmed}' is missing the patch component."))?;

        Ok(Version {
            major: component(major, "major")?,
            minor: component(minor, "minor")?,
            patch: component(patch, "patch")?,
            prerelease: prerelease
                .map(|text| identifiers(text, "prerelease"))
                .transpose()?
                .unwrap_or_default(),
            build: build
                .map(|text| identifiers(text, "build"))
                .transpose()?
                .unwrap_or_default(),
        })
    }

    fn release(major: u64, minor: u64, patch: u64) -> Version {
        Version {
            major,
            minor,
            patch,
            ..Version::default()
        }
    }
}

fn compare_identifier(left: &str, right: &str) -> Ordering {
    let numeric = |text: &str| !text.is_empty() && text.bytes().all(|b| b.is_ascii_digit());
    match (numeric(left), numeric(right)) {
        (true, true) => left.len().cmp(&right.len()).then_with(|| left.cmp(right)),
        (true, false) => Ordering::Less,
        (false, true) => Ordering::Greater,
        (false, false) => left.cmp(right),
    }
}

impl Ord for Version {
    fn cmp(&self, other: &Version) -> Ordering {
        (self.major, self.minor, self.patch)
            .cmp(&(other.major, other.minor, other.patch))
            .then_with(
                || match (self.prerelease.is_empty(), other.prerelease.is_empty()) {
                    (true, true) => Ordering::Equal,
                    (true, false) => Ordering::Greater,
                    (false, true) => Ordering::Less,
                    (false, false) => self
                        .prerelease
                        .iter()
                        .zip(&other.prerelease)
                        .map(|(left, right)| compare_identifier(left, right))
                        .find(|order| order.is_ne())
                        .unwrap_or_else(|| self.prerelease.len().cmp(&other.prerelease.len())),
                },
            )
    }
}

impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Version) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl PartialEq for Version {
    fn eq(&self, other: &Version) -> bool {
        self.cmp(other).is_eq()
    }
}

impl Eq for Version {}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)?;
        if !self.prerelease.is_empty() {
            write!(f, "-{}", self.prerelease.join("."))?;
        }
        if !self.build.is_empty() {
            write!(f, "+{}", self.build.join("."))?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Operator {
    Compatible,
    PatchCompatible,
    Equal,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
}

#[derive(Debug, Clone)]
struct Comparator {
    operator: Operator,
    version: Version,
}

impl Comparator {
    fn matches(&self, version: &Version) -> bool {
        let base = &self.version;
        match self.operator {
            // An upper bound has no prerelease, so ^1.0.0 admits 2.0.0-alpha;
            // the compiler's resolver does the same.
            Operator::Compatible => {
                let upper = if base.major > 0 {
                    Version::release(base.major + 1, 0, 0)
                } else if base.minor > 0 {
                    Version::release(0, base.minor + 1, 0)
                } else {
                    Version::release(0, 0, base.patch + 1)
                };
                version >= base && *version < upper
            }
            Operator::PatchCompatible => {
                version >= base && *version < Version::release(base.major, base.minor + 1, 0)
            }
            Operator::Equal => version == base,
            Operator::Greater => version > base,
            Operator::GreaterEqual => version >= base,
            Operator::Less => version < base,
            Operator::LessEqual => version <= base,
        }
    }
}

impl fmt::Display for Comparator {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let symbol = match self.operator {
            Operator::Compatible => "^",
            Operator::PatchCompatible => "~",
            Operator::Equal => "=",
            Operator::Greater => ">",
            Operator::GreaterEqual => ">=",
            Operator::Less => "<",
            Operator::LessEqual => "<=",
        };
        write!(f, "{symbol}{}", self.version)
    }
}

/// Comma-separated comparators, all of which must hold: `^1.2.0`,
/// `>=0.4.0, <0.5.0`. A bare version means `^`.
#[derive(Debug, Clone)]
pub struct Constraint {
    comparators: Vec<Comparator>,
}

impl Constraint {
    pub fn parse(text: &str) -> Result<Constraint, String> {
        let trimmed = text.trim();
        if trimmed.is_empty() {
            return Err("Constraint string is empty.".to_string());
        }

        let comparators = trimmed
            .split(',')
            .map(|segment| {
                let segment = segment.trim();
                if segment.is_empty() {
                    return Err(format!(
                        "Invalid empty comparator in constraint '{trimmed}'."
                    ));
                }
                let (operator, rest) = [
                    (">=", Operator::GreaterEqual),
                    ("<=", Operator::LessEqual),
                    ("^", Operator::Compatible),
                    ("~", Operator::PatchCompatible),
                    (">", Operator::Greater),
                    ("<", Operator::Less),
                    ("=", Operator::Equal),
                ]
                .iter()
                .find_map(|(prefix, operator)| {
                    segment.strip_prefix(prefix).map(|rest| (*operator, rest))
                })
                .unwrap_or((Operator::Compatible, segment));
                let version = Version::parse(rest)
                    .map_err(|error| format!("Invalid constraint '{segment}': {error}"))?;
                Ok(Comparator { operator, version })
            })
            .collect::<Result<Vec<_>, String>>()?;
        Ok(Constraint { comparators })
    }

    pub fn matches(&self, version: &Version) -> bool {
        self.comparators
            .iter()
            .all(|comparator| comparator.matches(version))
    }
}

impl fmt::Display for Constraint {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let parts: Vec<String> = self.comparators.iter().map(ToString::to_string).collect();
        write!(f, "{}", parts.join(", "))
    }
}

#[cfg(test)]
mod tests {
    use super::{Constraint, Version};

    fn v(text: &str) -> Version {
        Version::parse(text).unwrap()
    }

    fn allows(constraint: &str, version: &str) -> bool {
        Constraint::parse(constraint).unwrap().matches(&v(version))
    }

    #[test]
    fn versions_order_as_semver_says() {
        let ordered = [
            "1.0.0-alpha",
            "1.0.0-alpha.1",
            "1.0.0-alpha.beta",
            "1.0.0-beta",
            "1.0.0-beta.2",
            "1.0.0-beta.11",
            "1.0.0-rc.1",
            "1.0.0",
            "1.0.1",
            "1.1.0",
            "2.0.0",
        ];
        for pair in ordered.windows(2) {
            assert!(v(pair[0]) < v(pair[1]), "{} < {}", pair[0], pair[1]);
        }
        assert_eq!(v("1.0.0+build.1"), v("1.0.0+build.2"));
        assert_eq!(v(" 1.2.3-rc.1+b-9 ").to_string(), "1.2.3-rc.1+b-9");
    }

    #[test]
    fn malformed_versions_are_rejected() {
        for text in [
            "",
            "1",
            "1.2",
            "1.2.x",
            "1..2",
            "1.2.3-",
            "1.2.3-a..b",
            "1.2.3+",
            "a.b.c",
            "99999999999.0.0",
        ] {
            assert!(Version::parse(text).is_err(), "{text}");
        }
    }

    #[test]
    fn constraints_match_like_the_compiler() {
        assert!(allows("^1.2.0", "1.9.9"));
        assert!(!allows("^1.2.0", "2.0.0"));
        assert!(allows("^1.2.0", "2.0.0-alpha"));
        assert!(allows("^0.3.1", "0.3.9"));
        assert!(!allows("^0.3.1", "0.4.0"));
        assert!(allows("^0.0.3", "0.0.3"));
        assert!(!allows("^0.0.3", "0.0.4"));
        assert!(allows("1.2.0", "1.5.0"));
        assert!(allows("~1.2.3", "1.2.9"));
        assert!(!allows("~1.2.3", "1.3.0"));
        assert!(allows("=1.2.3", "1.2.3+meta"));
        assert!(allows(">=0.4.0, <0.5.0", "0.4.7"));
        assert!(!allows(">=0.4.0, <0.5.0", "0.5.0"));
        assert!(allows(">1.0.0", "1.0.1"));
        assert!(allows("<=1.0.0", "1.0.0"));
        assert_eq!(
            Constraint::parse(" >=0.4.0 ,<0.5.0").unwrap().to_string(),
            ">=0.4.0, <0.5.0"
        );
        assert_eq!(Constraint::parse("1.2.0").unwrap().to_string(), "^1.2.0");
        assert!(Constraint::parse("").is_err());
        assert!(Constraint::parse(">=1.0.0,").is_err());
        assert!(Constraint::parse("^x").is_err());
    }
}
