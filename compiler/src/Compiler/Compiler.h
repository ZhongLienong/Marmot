#pragma once

#include "Compiler/Module/CompiledModule.h"
#include "Compiler/Result/Result.h"
#include <memory>
#include <string>
#include <vector>

class MidoriType;

class Compiler
{
private:
	std::string m_source_code;
	std::string m_file_name;
	std::vector<std::string> m_source_lines;

	static bool InstanceTypeArgsEqual(const std::vector<std::shared_ptr<MidoriType>>& left, const std::vector<std::shared_ptr<MidoriType>>& right);

protected:
	static std::vector<std::string>& MergeInstanceMethods(std::vector<std::string>& target, const std::vector<std::string>& incoming);
	static std::vector<std::vector<std::shared_ptr<MidoriType>>>& MergeInstanceTypeArgs(std::vector<std::vector<std::shared_ptr<MidoriType>>>& target, const std::vector<std::vector<std::shared_ptr<MidoriType>>>& incoming);
	static std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>>& MergeInstanceAssociatedTypeBindings(
		std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>>& target_bindings,
		const std::vector<std::vector<std::shared_ptr<MidoriType>>>& target_type_args,
		const std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>>& incoming_bindings,
		const std::vector<std::vector<std::shared_ptr<MidoriType>>>& incoming_type_args
	);
	static bool TypeclassDefinitionsMatch(const CompiledModule::TypeclassMetadata& left, const CompiledModule::TypeclassMetadata& right);

public:
	Compiler(std::string&& source_code, std::string&& file_name);

	MidoriResult::CompilationResult CompileWithReport();
	MidoriResult::CompilerResult Compile();
};
