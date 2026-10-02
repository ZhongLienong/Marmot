#pragma once

#include "Compiler/Result/Result.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

struct ImportedSource
{
	TokenStream m_tokens;
	std::vector<std::string> m_source_lines;

	ImportedSource(TokenStream tokens, std::vector<std::string> source_lines);
};

class ModuleSourceLoader
{
public:
	enum class OpenFailure { File };
	using LoadResult = std::expected<ImportedSource, std::variant<OpenFailure, CompilerError>>;
	using Reader = std::function<LoadResult(const std::string&)>;

	explicit ModuleSourceLoader(size_t jobs);
	ModuleSourceLoader(size_t jobs, Reader reader);
	~ModuleSourceLoader();
	ModuleSourceLoader(const ModuleSourceLoader&) = delete;
	ModuleSourceLoader& operator=(const ModuleSourceLoader&) = delete;

	void Prefetch(const std::vector<std::string>& file_paths);
	MidoriResult::Result<ImportedSource> Take(const std::string& file_path, const std::string& importer, int line);

private:
	using LoadTask = std::packaged_task<LoadResult()>;

	Reader m_reader;
	size_t m_worker_limit;
	std::unordered_map<std::string, std::future<LoadResult>> m_pending;
	std::mutex m_mutex;
	std::condition_variable m_ready_cv;
	std::deque<LoadTask> m_ready;
	bool m_stop = false;
	// Futures and synchronization outlive every worker using them.
	std::vector<std::jthread> m_workers;

	static LoadResult Read(const std::string& file_path);
	std::optional<LoadTask> TakeReady();
	LoadResult Await(std::future<LoadResult> result);
	void WorkerLoop();
};
