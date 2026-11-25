#include "CompilationInputs.h"

#include <algorithm>
#include <utility>

namespace
{
	std::string SourcePathKey(const std::filesystem::path& path)
	{
		std::error_code error;
		std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
		std::string key = (error ? path.lexically_normal() : canonical).generic_string();
#ifdef _WIN32
		std::ranges::transform(key, key.begin(), [](char ch)
		{
			return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
		});
#endif
		return key;
	}
}

CompilationInputs CompilationInputs::WithSearchPaths(const std::vector<std::filesystem::path>& search_paths) &&
{
	m_search_paths.clear();
	for (const std::filesystem::path& path : search_paths)
	{
		std::error_code error;
		if (!std::filesystem::is_directory(path, error))
		{
			continue;
		}

		std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
		if (error)
		{
			continue;
		}

		m_search_paths.emplace_back(std::move(canonical));
	}

	return std::move(*this);
}

const std::vector<std::filesystem::path>& CompilationInputs::SearchPaths() const
{
	return m_search_paths;
}

CompilationInputs CompilationInputs::WithSourceOverlays(SourceOverlays overlays) &&
{
	SourceOverlays canonical;
	for (auto& [file_path, text] : overlays)
	{
		canonical.insert_or_assign(SourcePathKey(file_path), std::move(text));
	}
	m_source_overlays = std::make_shared<const SourceOverlays>(std::move(canonical));
	return std::move(*this);
}

const std::string* CompilationInputs::SourceOverlay(const std::filesystem::path& file_path) const
{
	if (!m_source_overlays)
	{
		return nullptr;
	}
	const SourceOverlays::const_iterator found = m_source_overlays->find(SourcePathKey(file_path));
	return found == m_source_overlays->end() ? nullptr : &found->second;
}

CompilationInputs CompilationInputs::WithNativeLibraryPolicies(std::unordered_map<std::string, NativeLibraryPolicy> policies) &&
{
	m_native_library_policies = std::move(policies);
	return std::move(*this);
}

const std::unordered_map<std::string, NativeLibraryPolicy>& CompilationInputs::NativeLibraryPolicies() const
{
	return m_native_library_policies;
}

CompilationInputs CompilationInputs::WithEmitMarmotIR(bool emit_marmot_ir) &&
{
	m_emit_marmot_ir = emit_marmot_ir;
	return std::move(*this);
}

bool CompilationInputs::EmitsMarmotIR() const
{
	return m_emit_marmot_ir;
}

CompilationInputs CompilationInputs::WithEmitAst(bool emit_ast) &&
{
	m_emit_ast = emit_ast;
	return std::move(*this);
}

bool CompilationInputs::EmitsAst() const
{
	return m_emit_ast;
}

CompilationInputs CompilationInputs::WithJobs(std::optional<size_t> jobs) &&
{
	m_jobs = jobs;
	return std::move(*this);
}

const std::optional<size_t>& CompilationInputs::Jobs() const
{
	return m_jobs;
}

CompilationInputs CompilationInputs::WithTimings(bool emit_timings) &&
{
	m_emit_timings = emit_timings;
	return std::move(*this);
}

bool CompilationInputs::EmitsTimings() const
{
	return m_emit_timings;
}
