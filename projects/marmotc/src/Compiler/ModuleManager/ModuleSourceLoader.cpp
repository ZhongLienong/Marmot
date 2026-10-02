#include "ModuleSourceLoader.h"

#include "Compiler/Lexer/Lexer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

ImportedSource::ImportedSource(TokenStream tokens, std::vector<std::string> source_lines)
	: m_tokens(std::move(tokens)),
	m_source_lines(std::move(source_lines))
{
}

ModuleSourceLoader::ModuleSourceLoader(size_t jobs)
	: ModuleSourceLoader(jobs, Read)
{
}

ModuleSourceLoader::ModuleSourceLoader(size_t jobs, Reader reader)
	: m_reader(std::move(reader)),
#ifndef __EMSCRIPTEN__
	m_worker_limit(jobs - 1u)
#else
	m_worker_limit(0u)
#endif
{
#ifdef __EMSCRIPTEN__
	(void)jobs;
#endif
}

ModuleSourceLoader::~ModuleSourceLoader()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stop = true;
		m_ready.clear();
	}
	m_ready_cv.notify_all();
}

ModuleSourceLoader::LoadResult ModuleSourceLoader::Read(const std::string& file_path)
{
	// Linux opens a directory as a stream that reads nothing.
	std::ifstream file(file_path);
	if (std::filesystem::is_directory(file_path) || !file.is_open())
	{
		return std::unexpected(OpenFailure::File);
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	std::string source = buffer.str();
	std::vector<std::string> source_lines;
	std::istringstream stream(source);
	std::string line;
	while (std::getline(stream, line))
	{
		source_lines.push_back(std::move(line));
	}
	MidoriResult::LexerResult lexed = Lexer(std::move(source), file_path).Lex();
	if (!lexed.has_value())
	{
		return std::unexpected(std::move(lexed.error()));
	}
	return ImportedSource(std::move(lexed).value(), std::move(source_lines));
}

void ModuleSourceLoader::Prefetch(const std::vector<std::string>& file_paths)
{
	if (m_worker_limit == 0u || file_paths.empty())
	{
		return;
	}
	size_t queued = 0u;
	size_t added = 0u;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const std::string& file_path : file_paths)
		{
			if (!m_pending.contains(file_path))
			{
				LoadTask task([this, file_path]() { return m_reader(file_path); });
				m_pending.emplace(file_path, task.get_future());
				m_ready.push_back(std::move(task));
				added += 1u;
			}
		}
		queued = m_ready.size();
	}
	if (added == 0u)
	{
		return;
	}
#ifndef __EMSCRIPTEN__
	// The coordinator loads too, so the shared job budget includes it. A
	// chain creates no threads until discovery finds independent files.
	const size_t workers = std::min(m_worker_limit, queued > 0u ? queued - 1u : 0u);
	while (m_workers.size() < workers)
	{
		m_workers.emplace_back([this]() { WorkerLoop(); });
	}
#else
	(void)queued;
#endif
	if (queued == 1u)
	{
		m_ready_cv.notify_one();
	}
	else
	{
		m_ready_cv.notify_all();
	}
}

std::optional<ModuleSourceLoader::LoadTask> ModuleSourceLoader::TakeReady()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_ready.empty())
	{
		return std::nullopt;
	}
	LoadTask task = std::move(m_ready.front());
	m_ready.pop_front();
	return task;
}

ModuleSourceLoader::LoadResult ModuleSourceLoader::Await(std::future<LoadResult> result)
{
	while (result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		std::optional<LoadTask> task = TakeReady();
		if (!task.has_value())
		{
			result.wait();
			break;
		}
		task.value()();
	}
	return result.get();
}

MidoriResult::Result<ImportedSource> ModuleSourceLoader::Take(const std::string& file_path, const std::string& importer, int line)
{
	LoadResult loaded = m_worker_limit == 0u
		? m_reader(file_path)
		: Await(std::move(m_pending.extract(file_path).mapped()));
	if (loaded.has_value())
	{
		return std::move(loaded).value();
	}
	if (std::holds_alternative<OpenFailure>(loaded.error()))
	{
		// The first importer in traversal order owns this diagnostic, even
		// when another importer prefetched the same file first.
		return std::unexpected(MidoriError::GenerateModuleErrorWithContext(
			CompilerErrorCode::ModuleImportFileOpenFailed, "Could not open import file: " + file_path, line, importer));
	}
	return std::unexpected(std::move(std::get<CompilerError>(loaded.error())));
}

void ModuleSourceLoader::WorkerLoop()
{
	while (true)
	{
		std::optional<LoadTask> task;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_ready_cv.wait(lock, [this]() { return m_stop || !m_ready.empty(); });
			if (m_stop)
			{
				return;
			}
			task.emplace(std::move(m_ready.front()));
			m_ready.pop_front();
		}
		task.value()();
	}
}
