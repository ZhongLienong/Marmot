#include "Utility/BuildPlan/BuildPlan.h"

#include "Common/Json/JsonReader.h"

#include <cmath>
#include <format>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <vector>

namespace
{
	using MidoriJson::JsonValue;

	template<typename T>
	using PlanResult = std::expected<T, std::string>;

	PlanResult<void> RequireOnlyMembers(const JsonValue& object, std::string_view where, std::initializer_list<std::string_view> allowed)
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
				return std::unexpected(std::format("{}: unknown member \"{}\"", where, member.first));
			}
		}

		return {};
	}

	PlanResult<const JsonValue*> RequireMember(const JsonValue& object, std::string_view where, std::string_view name)
	{
		const JsonValue* value = object.Find(name);
		if (value == nullptr)
		{
			return std::unexpected(std::format("{}: missing \"{}\"", where, name));
		}

		return value;
	}

	PlanResult<std::string> RequireString(const JsonValue& value, std::string_view where)
	{
		if (!value.IsString())
		{
			return std::unexpected(std::format("{}: expected a string, found {}", where, value.KindName()));
		}

		return value.AsString();
	}

	std::filesystem::path Resolve(const std::filesystem::path& base_directory, const std::string& text)
	{
		const std::filesystem::path path(text);
		return path.is_absolute() ? path : base_directory / path;
	}

	PlanResult<std::vector<std::filesystem::path>> ReadDirectories(const JsonValue* value, std::string_view member, const std::filesystem::path& base_directory)
	{
		std::vector<std::filesystem::path> search_paths;
		if (value == nullptr)
		{
			return search_paths;
		}

		if (!value->IsArray())
		{
			return std::unexpected(std::format("{}: expected an array, found {}", member, value->KindName()));
		}

		for (size_t index = 0uz; index < value->AsArray().size(); index += 1uz)
		{
			const std::string where = std::format("{}[{}]", member, index);
			PlanResult<std::string> text = RequireString(value->AsArray()[index], where);
			if (!text.has_value())
			{
				return std::unexpected(text.error());
			}

			const std::filesystem::path path = Resolve(base_directory, text.value());
			std::error_code error;
			if (!std::filesystem::is_directory(path, error))
			{
				return std::unexpected(std::format("{}: not a directory: {}", where, path.string()));
			}
			search_paths.push_back(path);
		}

		return search_paths;
	}

	PlanResult<std::pair<std::string, NativeLibraryPolicy>> ReadNativeLibrary(const JsonValue& value, std::string_view where)
	{
		if (!value.IsObject())
		{
			return std::unexpected(std::format("{}: expected an object, found {}", where, value.KindName()));
		}

		PlanResult<void> members = RequireOnlyMembers(value, where, { "name", "checksum", "thread_safe" });
		if (!members.has_value())
		{
			return std::unexpected(members.error());
		}

		PlanResult<const JsonValue*> name = RequireMember(value, where, "name");
		if (!name.has_value())
		{
			return std::unexpected(name.error());
		}
		PlanResult<std::string> name_text = RequireString(*name.value(), std::format("{}.name", where));
		if (!name_text.has_value())
		{
			return std::unexpected(name_text.error());
		}

		NativeLibraryPolicy settings;

		if (const JsonValue* checksum = value.Find("checksum"); checksum != nullptr)
		{
			PlanResult<std::string> text = RequireString(*checksum, std::format("{}.checksum", where));
			if (!text.has_value())
			{
				return std::unexpected(text.error());
			}
			settings.m_checksum = text.value();
		}

		if (const JsonValue* thread_safe = value.Find("thread_safe"); thread_safe != nullptr)
		{
			if (!thread_safe->IsBool())
			{
				return std::unexpected(std::format("{}.thread_safe: expected a boolean, found {}", where, thread_safe->KindName()));
			}
			settings.m_thread_safe = thread_safe->AsBool();
		}

		return std::pair<std::string, NativeLibraryPolicy>(name_text.value(), std::move(settings));
	}

	PlanResult<std::unordered_map<std::string, NativeLibraryPolicy>> ReadNativeLibraries(const JsonValue* value)
	{
		std::unordered_map<std::string, NativeLibraryPolicy> libraries;
		if (value == nullptr)
		{
			return libraries;
		}

		if (!value->IsArray())
		{
			return std::unexpected(std::format("native_libraries: expected an array, found {}", value->KindName()));
		}

		for (size_t index = 0uz; index < value->AsArray().size(); index += 1uz)
		{
			PlanResult<std::pair<std::string, NativeLibraryPolicy>> library = ReadNativeLibrary(value->AsArray()[index], std::format("native_libraries[{}]", index));
			if (!library.has_value())
			{
				return std::unexpected(library.error());
			}

			if (!libraries.emplace(library->first, std::move(library->second)).second)
			{
				return std::unexpected(std::format("native_libraries[{}]: a second library named \"{}\"", index, library->first));
			}
		}

		return libraries;
	}
}

namespace MidoriBuildPlan
{
	std::expected<BuildPlan, std::string> Parse(std::string_view json, const std::filesystem::path& base_directory)
	{
		const std::expected<JsonValue, std::string> document = MidoriJson::Parse(json);
		if (!document.has_value())
		{
			return std::unexpected(document.error());
		}

		const JsonValue& root = document.value();
		if (!root.IsObject())
		{
			return std::unexpected(std::format("the plan must be a JSON object, found {}", root.KindName()));
		}

		PlanResult<void> members = RequireOnlyMembers(root, "plan", { "version", "entry", "search_paths", "native_libraries" });
		if (!members.has_value())
		{
			return std::unexpected(members.error());
		}

		PlanResult<const JsonValue*> version = RequireMember(root, "plan", "version");
		if (!version.has_value())
		{
			return std::unexpected(version.error());
		}
		const JsonValue& version_value = *version.value();
		if (!version_value.IsNumber() || version_value.AsNumber() != static_cast<double>(BuildPlan::VERSION))
		{
			return std::unexpected(std::format("version: this compiler reads plan version {}; the plan says {}",
				BuildPlan::VERSION, version_value.IsNumber() ? std::format("{}", version_value.AsNumber()) : std::string(version_value.KindName())));
		}

		BuildPlan plan;
		if (const JsonValue* entry = root.Find("entry"); entry != nullptr)
		{
			PlanResult<std::string> entry_text = RequireString(*entry, "entry");
			if (!entry_text.has_value())
			{
				return std::unexpected(entry_text.error());
			}

			plan.m_entry = Resolve(base_directory, entry_text.value());
			std::error_code error;
			if (!std::filesystem::is_regular_file(plan.m_entry.value(), error))
			{
				return std::unexpected(std::format("entry: no such file: {}", plan.m_entry->string()));
			}
		}

		PlanResult<std::vector<std::filesystem::path>> search_paths = ReadDirectories(root.Find("search_paths"), "search_paths", base_directory);
		if (!search_paths.has_value())
		{
			return std::unexpected(search_paths.error());
		}

		PlanResult<std::unordered_map<std::string, NativeLibraryPolicy>> native_libraries = ReadNativeLibraries(root.Find("native_libraries"));
		if (!native_libraries.has_value())
		{
			return std::unexpected(native_libraries.error());
		}

		plan.m_inputs = CompilationInputs()
			.WithSearchPaths(search_paths.value())
			.WithNativeLibraryPolicies(std::move(native_libraries.value()));
		return plan;
	}

	std::expected<BuildPlan, std::string> ReadFile(const std::filesystem::path& plan_path)
	{
		std::ifstream file(plan_path, std::ios::binary);
		if (!file.is_open())
		{
			return std::unexpected(std::format("cannot open build plan: {}", plan_path.string()));
		}

		std::ostringstream contents;
		contents << file.rdbuf();

		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(plan_path, error);
		std::expected<BuildPlan, std::string> plan = Parse(contents.str(), (error ? plan_path : absolute).parent_path());
		if (!plan.has_value())
		{
			return std::unexpected(std::format("{}: {}", plan_path.string(), plan.error()));
		}

		return plan;
	}
}
