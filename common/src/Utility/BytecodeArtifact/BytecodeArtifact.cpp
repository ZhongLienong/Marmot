#include "Utility/BytecodeArtifact/BytecodeArtifact.h"

#include <fstream>
#include <format>

#include "Common/Json/Json.h"

namespace
{
	[[nodiscard]] std::filesystem::path ArtifactPathForSource(const std::filesystem::path& source_file)
	{
		std::filesystem::path artifact_path = source_file;
		artifact_path.replace_extension(".mmc.json");
		return artifact_path;
	}

	[[nodiscard]] std::string SerializeStringArray(const std::vector<std::string>& values)
	{
		std::string serialized = "[";
		for (size_t index = 0u; index < values.size(); index += 1u)
		{
			if (index > 0u)
			{
				serialized.push_back(',');
			}

			serialized.push_back('\"');
			serialized += MidoriJson::EscapeString(values[index]);
			serialized.push_back('\"');
		}
		serialized.push_back(']');
		return serialized;
	}

	[[nodiscard]] std::string SerializeProcedure(const MidoriExecutable& executable, int proc_index)
	{
		const int instruction_count = executable.GetByteCodeSize(proc_index);
		std::string opcodes_json = "[";
		std::string lines_json = "[";
		for (int instruction_index = 0; instruction_index < instruction_count; instruction_index += 1)
		{
			if (instruction_index > 0)
			{
				opcodes_json.push_back(',');
				lines_json.push_back(',');
			}

			opcodes_json += std::to_string(static_cast<int>(executable.ReadByteCode(instruction_index, proc_index)));
			lines_json += std::to_string(executable.GetLine(instruction_index, proc_index));
		}
		opcodes_json.push_back(']');
		lines_json.push_back(']');

		std::string object = "{";
		bool first_field = true;
		MidoriJson::AppendNumberField(object, "index", proc_index, first_field);
		MidoriJson::AppendStringField(object, "name", executable.m_procedure_names[static_cast<size_t>(proc_index)], first_field);
		MidoriJson::AppendNumberField(object, "instructionCount", instruction_count, first_field);
		MidoriJson::AppendRawField(object, "opcodes", opcodes_json, first_field);
		MidoriJson::AppendRawField(object, "lines", lines_json, first_field);
		object.push_back('}');
		return object;
	}
}

namespace MidoriBytecodeArtifact
{
	std::expected<ArtifactResult, std::string> WriteExecutableArtifact(
		const MidoriExecutable& executable,
		const std::filesystem::path& source_file)
	{
		const std::filesystem::path artifact_path = ArtifactPathForSource(source_file);

		std::vector<std::string> globals;
		globals.reserve(static_cast<size_t>(executable.GetGlobalVariableCount()));
		for (int index = 0; index < executable.GetGlobalVariableCount(); index += 1)
		{
			globals.emplace_back(executable.GetGlobalVariable(index));
		}

		const std::vector<std::string> strings = executable.GetStringPool();

		int total_instruction_count = 0;
		std::string procedures_json = "[";
		for (int proc_index = 0; proc_index < executable.GetProcedureCount(); proc_index += 1)
		{
			if (proc_index > 0)
			{
				procedures_json.push_back(',');
			}

			total_instruction_count += executable.GetByteCodeSize(proc_index);
			procedures_json += SerializeProcedure(executable, proc_index);
		}
		procedures_json.push_back(']');

		std::string artifact_json = "{";
		bool first_field = true;
		MidoriJson::AppendNumberField(artifact_json, "version", 1, first_field);
		MidoriJson::AppendStringField(artifact_json, "kind", "marmot-bytecode", first_field);
		MidoriJson::AppendStringField(artifact_json, "path", artifact_path.generic_string(), first_field);
		MidoriJson::AppendStringField(artifact_json, "entryFile", source_file.generic_string(), first_field);
		MidoriJson::AppendNumberField(artifact_json, "procedureCount", executable.GetProcedureCount(), first_field);
		MidoriJson::AppendNumberField(artifact_json, "globalCount", executable.GetGlobalVariableCount(), first_field);
		MidoriJson::AppendNumberField(artifact_json, "stringCount", static_cast<int>(strings.size()), first_field);
		MidoriJson::AppendNumberField(artifact_json, "instructionCount", total_instruction_count, first_field);
		MidoriJson::AppendRawField(artifact_json, "globals", SerializeStringArray(globals), first_field);
		MidoriJson::AppendRawField(artifact_json, "strings", SerializeStringArray(strings), first_field);
		MidoriJson::AppendRawField(artifact_json, "procedures", procedures_json, first_field);
		artifact_json.push_back('}');

		std::ofstream output(artifact_path, std::ios::binary | std::ios::trunc);
		if (!output.is_open())
		{
			return std::unexpected(std::format("Could not open bytecode artifact for writing: {}", artifact_path.string()));
		}

		output.write(artifact_json.data(), static_cast<std::streamsize>(artifact_json.size()));
		if (!output)
		{
			return std::unexpected(std::format("Could not write bytecode artifact: {}", artifact_path.string()));
		}

		return ArtifactResult
		{
			.m_path = artifact_path,
			.m_json = artifact_json
		};
	}
}
