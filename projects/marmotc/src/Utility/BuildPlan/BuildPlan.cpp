#include "Utility/BuildPlan/BuildPlan.h"

#include "Compiler/Json/JsonReader.h"
#include "Support/Expected.h"

#include <cmath>
#include <format>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <vector>

namespace
{
	using CompilerJson::JsonValue;

	template<typename T>
	using FieldResult = MarmotCompiler::Expected<T, std::string>;

	FieldResult<void> RequireOnlyMembers(const JsonValue& object, std::string_view where, std::initializer_list<std::string_view> allowed)
	{
		for (const std::pair<std::string, JsonValue>& member : object.AsObject())
		{
			bool known = false;
			for (std::string_view name : allowed)
			{
				if (member.first == name)
				{
					known = true;
					break;
				}
			}

			if (!known)
			{
				return MarmotCompiler::Failure(std::format("{}: unknown member \"{}\"", where, member.first));
			}
		}

		return {};
	}

	FieldResult<const JsonValue*> RequireMember(const JsonValue& object, std::string_view where, std::string_view name)
	{
		const JsonValue* value = object.Find(name);
		if (value == nullptr)
		{
			return MarmotCompiler::Failure(std::format("{}: missing \"{}\"", where, name));
		}

		return value;
	}

	FieldResult<std::string> RequireString(const JsonValue& value, std::string_view where)
	{
		if (!value.IsString())
		{
			return MarmotCompiler::Failure(std::format("{}: expected a string, found {}", where, value.KindName()));
		}

		return value.AsString();
	}

	std::filesystem::path Resolve(const std::filesystem::path& base_directory, const std::string& text)
	{
		const std::filesystem::path path(text);
		return path.is_absolute() ? path : base_directory / path;
	}

	FieldResult<std::vector<std::filesystem::path>> ReadDirectories(const JsonValue* value, std::string_view member, const std::filesystem::path& base_directory)
	{
		std::vector<std::filesystem::path> search_paths;
		if (value == nullptr)
		{
			return search_paths;
		}

		if (!value->IsArray())
		{
			return MarmotCompiler::Failure(std::format("{}: expected an array, found {}", member, value->KindName()));
		}

		for (size_t index = 0uz; index < value->AsArray().size(); index += 1uz)
		{
			const std::string where = std::format("{}[{}]", member, index);
			FieldResult<std::string> text = RequireString(value->AsArray()[index], where);
			if (!text.HasValue())
			{
				return MarmotCompiler::Failure(text.Error());
			}

			const std::filesystem::path path = Resolve(base_directory, text.Value());
			std::error_code error;
			if (!std::filesystem::is_directory(path, error))
			{
				return MarmotCompiler::Failure(std::format("{}: not a directory: {}", where, path.string()));
			}
			search_paths.push_back(path);
		}

		return search_paths;
	}

	FieldResult<std::pair<std::string, NativeLibraryPolicy>> ReadNativeLibrary(const JsonValue& value, std::string_view where)
	{
		if (!value.IsObject())
		{
			return MarmotCompiler::Failure(std::format("{}: expected an object, found {}", where, value.KindName()));
		}

		FieldResult<void> members = RequireOnlyMembers(value, where, { "name", "checksum", "thread_safe" });
		if (!members.HasValue())
		{
			return MarmotCompiler::Failure(members.Error());
		}

		FieldResult<const JsonValue*> name = RequireMember(value, where, "name");
		if (!name.HasValue())
		{
			return MarmotCompiler::Failure(name.Error());
		}
		FieldResult<std::string> name_text = RequireString(*name.Value(), std::format("{}.name", where));
		if (!name_text.HasValue())
		{
			return MarmotCompiler::Failure(name_text.Error());
		}

		NativeLibraryPolicy settings;

		if (const JsonValue* checksum = value.Find("checksum"); checksum != nullptr)
		{
			FieldResult<std::string> text = RequireString(*checksum, std::format("{}.checksum", where));
			if (!text.HasValue())
			{
				return MarmotCompiler::Failure(text.Error());
			}
			settings.m_checksum = text.Value();
		}

		if (const JsonValue* thread_safe = value.Find("thread_safe"); thread_safe != nullptr)
		{
			if (!thread_safe->IsBool())
			{
				return MarmotCompiler::Failure(std::format("{}.thread_safe: expected a boolean, found {}", where, thread_safe->KindName()));
			}
			settings.m_thread_safe = thread_safe->AsBool();
		}

		return std::pair<std::string, NativeLibraryPolicy>(name_text.Value(), std::move(settings));
	}

	FieldResult<std::unordered_map<std::string, NativeLibraryPolicy>> ReadNativeLibraries(const JsonValue* value)
	{
		std::unordered_map<std::string, NativeLibraryPolicy> libraries;
		if (value == nullptr)
		{
			return libraries;
		}

		if (!value->IsArray())
		{
			return MarmotCompiler::Failure(std::format("native_libraries: expected an array, found {}", value->KindName()));
		}

		for (size_t index = 0uz; index < value->AsArray().size(); index += 1uz)
		{
			FieldResult<std::pair<std::string, NativeLibraryPolicy>> library = ReadNativeLibrary(value->AsArray()[index], std::format("native_libraries[{}]", index));
			if (!library.HasValue())
			{
				return MarmotCompiler::Failure(library.Error());
			}

			if (!libraries.emplace(library->first, std::move(library->second)).second)
			{
				return MarmotCompiler::Failure(std::format("native_libraries[{}]: a second library named \"{}\"", index, library->first));
			}
		}

		return libraries;
	}
}

namespace MarmotBuildPlan
{
	PlanResult Parse(std::string_view json, const std::filesystem::path& base_directory)
	{
		const MarmotCompiler::Expected<JsonValue, std::string> document = CompilerJson::Parse(json);
		if (!document.HasValue())
		{
			return MarmotCompiler::Failure(document.Error());
		}

		return Parse(document.Value(), base_directory);
	}

	PlanResult Parse(const JsonValue& root, const std::filesystem::path& base_directory)
	{
		if (!root.IsObject())
		{
			return MarmotCompiler::Failure(std::format("the plan must be a JSON object, found {}", root.KindName()));
		}

		FieldResult<void> members = RequireOnlyMembers(root, "plan", { "version", "entry", "search_paths", "native_libraries" });
		if (!members.HasValue())
		{
			return MarmotCompiler::Failure(members.Error());
		}

		FieldResult<const JsonValue*> version = RequireMember(root, "plan", "version");
		if (!version.HasValue())
		{
			return MarmotCompiler::Failure(version.Error());
		}
		const JsonValue& version_value = *version.Value();
		if (!version_value.IsNumber() || version_value.AsNumber() != static_cast<double>(BuildPlan::VERSION))
		{
			return MarmotCompiler::Failure(std::format("version: this compiler reads plan version {}; the plan says {}",
				BuildPlan::VERSION, version_value.IsNumber() ? std::format("{}", version_value.AsNumber()) : std::string(version_value.KindName())));
		}

		BuildPlan plan;
		if (const JsonValue* entry = root.Find("entry"); entry != nullptr)
		{
			FieldResult<std::string> entry_text = RequireString(*entry, "entry");
			if (!entry_text.HasValue())
			{
				return MarmotCompiler::Failure(entry_text.Error());
			}

			plan.m_entry = Resolve(base_directory, entry_text.Value());
			std::error_code error;
			if (!std::filesystem::is_regular_file(plan.m_entry.Value(), error))
			{
				return MarmotCompiler::Failure(std::format("entry: no such file: {}", plan.m_entry->string()));
			}
		}

		FieldResult<std::vector<std::filesystem::path>> search_paths = ReadDirectories(root.Find("search_paths"), "search_paths", base_directory);
		if (!search_paths.HasValue())
		{
			return MarmotCompiler::Failure(search_paths.Error());
		}

		FieldResult<std::unordered_map<std::string, NativeLibraryPolicy>> native_libraries = ReadNativeLibraries(root.Find("native_libraries"));
		if (!native_libraries.HasValue())
		{
			return MarmotCompiler::Failure(native_libraries.Error());
		}

		plan.m_inputs = CompilationInputs()
			.WithSearchPaths(search_paths.Value())
			.WithNativeLibraryPolicies(std::move(native_libraries.Value()));
		return plan;
	}

	PlanResult ReadFile(const std::filesystem::path& plan_path)
	{
		std::ifstream file(plan_path, std::ios::binary);
		if (!file.is_open())
		{
			return MarmotCompiler::Failure(std::format("cannot open build plan: {}", plan_path.string()));
		}

		std::ostringstream contents;
		contents << file.rdbuf();

		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(plan_path, error);
		PlanResult plan = Parse(contents.str(), (error ? plan_path : absolute).parent_path());
		if (!plan.HasValue())
		{
			return MarmotCompiler::Failure(std::format("{}: {}", plan_path.string(), plan.Error()));
		}

		return plan;
	}
}
