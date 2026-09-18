#pragma once

#include <compare>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace MidoriVersion
{
	struct SemanticVersion
	{
		int m_major = 0;
		int m_minor = 0;
		int m_patch = 0;
		std::vector<std::string> m_prerelease;
		std::vector<std::string> m_build;

		[[nodiscard]] static std::expected<SemanticVersion, std::string> Parse(std::string_view text);

		[[nodiscard]] std::string ToString() const;

		[[nodiscard]] std::strong_ordering operator<=>(const SemanticVersion& other) const noexcept;

		[[nodiscard]] bool operator==(const SemanticVersion& other) const noexcept;
	};

	class VersionConstraint
	{
	public:
		enum class Operator
		{
			Compatible,
			PatchCompatible,
			Equal,
			Greater,
			GreaterEqual,
			Less,
			LessEqual
		};

		struct Comparator
		{
			Operator m_operator = Operator::Compatible;
			SemanticVersion m_version;

			[[nodiscard]] bool Matches(const SemanticVersion& version) const;

			[[nodiscard]] std::string ToString() const;
		};

		[[nodiscard]] static std::expected<VersionConstraint, std::string> Parse(std::string_view text);

		[[nodiscard]] bool Matches(const SemanticVersion& version) const;

		[[nodiscard]] bool Empty() const noexcept;

		[[nodiscard]] std::string ToString() const;

		[[nodiscard]] const std::vector<Comparator>& GetComparators() const noexcept;

	private:
		std::vector<Comparator> m_comparators;
	};
}
