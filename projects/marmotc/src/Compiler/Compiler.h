#pragma once

#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/Result/Result.h"
#include <memory>
#include <string>
#include <vector>

class MarmotType;
class CompilationTimings;

class Compiler
{
private:
	std::string m_source_code;
	std::string m_file_name;
	std::vector<std::string> m_source_lines;
	CompilationInputs m_inputs;

	MarmotResult::CompilationResult CompileWithTimings(CompilationTimings& timings);

	static bool InstanceTypeArgsEqual(const std::vector<std::shared_ptr<MarmotType>>& left, const std::vector<std::shared_ptr<MarmotType>>& right);

protected:
	static std::vector<std::string>& MergeInstanceMethods(std::vector<std::string>& target, const std::vector<std::string>& incoming);
	static std::vector<std::vector<std::shared_ptr<MarmotType>>>& MergeInstanceTypeArgs(std::vector<std::vector<std::shared_ptr<MarmotType>>>& target, const std::vector<std::vector<std::shared_ptr<MarmotType>>>& incoming);
	template <typename Entry>
	static std::vector<Entry>& MergeInstanceEntries(
		std::vector<Entry>& target_entries,
		const std::vector<std::vector<std::shared_ptr<MarmotType>>>& target_type_args,
		const std::vector<Entry>& incoming_entries,
		const std::vector<std::vector<std::shared_ptr<MarmotType>>>& incoming_type_args
	);
	static bool TypeclassDefinitionsMatch(const CompiledModule::TypeclassMetadata& left, const CompiledModule::TypeclassMetadata& right);

public:
	Compiler(std::string&& source_code, std::string&& file_name, CompilationInputs inputs = {});

	MarmotResult::CompilationResult CompileWithReport();
	MarmotResult::CompilerResult Compile();
};
