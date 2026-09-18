#include "Common/Version/Version.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace
{
	[[nodiscard]] std::string Trim(std::string_view text)
	{
		size_t start = 0u;
		size_t end = text.size();

		while (start < end && std::isspace(static_cast<unsigned char>(text[start])) != 0)
		{
			start += 1u;
		}

		while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1u])) != 0)
		{
			end -= 1u;
		}

		return std::string(text.substr(start, end - start));
	}

	[[nodiscard]] bool IsIdentifierCharacter(char c)
	{
		const unsigned char value = static_cast<unsigned char>(c);
		return std::isalnum(value) != 0 || c == '-';
	}

	[[nodiscard]] bool IsNumericIdentifier(std::string_view identifier)
	{
		if (identifier.empty())
		{
			return false;
		}

		for (const char c : identifier)
		{
			if (std::isdigit(static_cast<unsigned char>(c)) == 0)
			{
				return false;
			}
		}

		return true;
	}

	[[nodiscard]] std::expected<int, std::string> ParseIntegerComponent(std::string_view text, std::string_view label)
	{
		if (text.empty())
		{
			return std::unexpected(std::format("Missing {} component.", label));
		}

		for (const char c : text)
		{
			if (std::isdigit(static_cast<unsigned char>(c)) == 0)
			{
				return std::unexpected(std::format("Invalid {} component '{}'.", label, text));
			}
		}

		int value = 0;
		const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
		if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
		{
			return std::unexpected(std::format("Invalid {} component '{}'.", label, text));
		}

		return value;
	}

	[[nodiscard]] std::expected<std::vector<std::string>, std::string> ParseIdentifiers(std::string_view text, std::string_view label)
	{
		std::vector<std::string> identifiers;
		size_t start = 0u;

		while (start <= text.size())
		{
			const size_t end = text.find('.', start);
			const std::string_view token = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
			if (token.empty())
			{
				return std::unexpected(std::format("Invalid empty {} identifier.", label));
			}

			for (const char c : token)
			{
				if (!IsIdentifierCharacter(c))
				{
					return std::unexpected(std::format("Invalid {} identifier '{}'.", label, token));
				}
			}

			identifiers.emplace_back(token);
			if (end == std::string_view::npos)
			{
				break;
			}

			start = end + 1u;
		}

		return identifiers;
	}

	[[nodiscard]] std::strong_ordering CompareIdentifier(std::string_view left, std::string_view right) noexcept
	{
		const bool left_numeric = IsNumericIdentifier(left);
		const bool right_numeric = IsNumericIdentifier(right);

		if (left_numeric && right_numeric)
		{
			if (left.size() != right.size())
			{
				return left.size() < right.size() ? std::strong_ordering::less : std::strong_ordering::greater;
			}

			if (left < right)
			{
				return std::strong_ordering::less;
			}
			if (left > right)
			{
				return std::strong_ordering::greater;
			}
			return std::strong_ordering::equal;
		}

		if (left_numeric != right_numeric)
		{
			return left_numeric ? std::strong_ordering::less : std::strong_ordering::greater;
		}

		if (left < right)
		{
			return std::strong_ordering::less;
		}
		if (left > right)
		{
			return std::strong_ordering::greater;
		}
		return std::strong_ordering::equal;
	}

	[[nodiscard]] MidoriVersion::SemanticVersion IncrementCompatibleUpperBound(const MidoriVersion::SemanticVersion& version)
	{
		MidoriVersion::SemanticVersion upper_bound;
		if (version.m_major > 0)
		{
			upper_bound.m_major = version.m_major + 1;
			return upper_bound;
		}

		if (version.m_minor > 0)
		{
			upper_bound.m_major = 0;
			upper_bound.m_minor = version.m_minor + 1;
			return upper_bound;
		}

		upper_bound.m_patch = version.m_patch + 1;
		return upper_bound;
	}

	[[nodiscard]] MidoriVersion::SemanticVersion IncrementPatchCompatibleUpperBound(const MidoriVersion::SemanticVersion& version)
	{
		MidoriVersion::SemanticVersion upper_bound;
		upper_bound.m_major = version.m_major;
		upper_bound.m_minor = version.m_minor + 1;
		return upper_bound;
	}
}

namespace MidoriVersion
{
	std::expected<SemanticVersion, std::string> SemanticVersion::Parse(std::string_view text)
	{
		const std::string trimmed = Trim(text);
		if (trimmed.empty())
		{
			return std::unexpected("Version string is empty.");
		}

		const size_t build_separator = trimmed.find('+');
		const size_t prerelease_separator = trimmed.find('-');
		const size_t core_end = std::min(
			prerelease_separator == std::string::npos ? trimmed.size() : prerelease_separator,
			build_separator == std::string::npos ? trimmed.size() : build_separator);

		const std::string_view core = std::string_view(trimmed).substr(0u, core_end);
		size_t start = 0u;
		size_t dot = core.find('.');
		if (dot == std::string_view::npos)
		{
			return std::unexpected(std::format("Version '{}' is missing the minor or patch component.", trimmed));
		}

		const std::expected<int, std::string> major = ParseIntegerComponent(core.substr(start, dot - start), "major");
		if (!major.has_value())
		{
			return std::unexpected(major.error());
		}

		start = dot + 1u;
		dot = core.find('.', start);
		if (dot == std::string_view::npos)
		{
			return std::unexpected(std::format("Version '{}' is missing the patch component.", trimmed));
		}

		const std::expected<int, std::string> minor = ParseIntegerComponent(core.substr(start, dot - start), "minor");
		if (!minor.has_value())
		{
			return std::unexpected(minor.error());
		}

		const std::expected<int, std::string> patch = ParseIntegerComponent(core.substr(dot + 1u), "patch");
		if (!patch.has_value())
		{
			return std::unexpected(patch.error());
		}

		SemanticVersion version;
		version.m_major = major.value();
		version.m_minor = minor.value();
		version.m_patch = patch.value();

		if (prerelease_separator != std::string::npos)
		{
			const size_t prerelease_end = build_separator == std::string::npos ? trimmed.size() : build_separator;
			const std::expected<std::vector<std::string>, std::string> prerelease = ParseIdentifiers(
				std::string_view(trimmed).substr(prerelease_separator + 1u, prerelease_end - prerelease_separator - 1u),
				"prerelease");
			if (!prerelease.has_value())
			{
				return std::unexpected(prerelease.error());
			}

			version.m_prerelease = prerelease.value();
		}

		if (build_separator != std::string::npos)
		{
			const std::expected<std::vector<std::string>, std::string> build = ParseIdentifiers(
				std::string_view(trimmed).substr(build_separator + 1u),
				"build");
			if (!build.has_value())
			{
				return std::unexpected(build.error());
			}

			version.m_build = build.value();
		}

		return version;
	}

	std::string SemanticVersion::ToString() const
	{
		std::string text = std::format("{}.{}.{}", m_major, m_minor, m_patch);
		if (!m_prerelease.empty())
		{
			text.push_back('-');
			for (size_t index = 0u; index < m_prerelease.size(); index += 1u)
			{
				if (index != 0u)
				{
					text.push_back('.');
				}
				text.append(m_prerelease[index]);
			}
		}

		if (!m_build.empty())
		{
			text.push_back('+');
			for (size_t index = 0u; index < m_build.size(); index += 1u)
			{
				if (index != 0u)
				{
					text.push_back('.');
				}
				text.append(m_build[index]);
			}
		}

		return text;
	}

	std::strong_ordering SemanticVersion::operator<=>(const SemanticVersion& other) const noexcept
	{
		if (const std::strong_ordering major_order = m_major <=> other.m_major; major_order != 0)
		{
			return major_order;
		}

		if (const std::strong_ordering minor_order = m_minor <=> other.m_minor; minor_order != 0)
		{
			return minor_order;
		}

		if (const std::strong_ordering patch_order = m_patch <=> other.m_patch; patch_order != 0)
		{
			return patch_order;
		}

		if (m_prerelease.empty() && other.m_prerelease.empty())
		{
			return std::strong_ordering::equal;
		}

		if (m_prerelease.empty())
		{
			return std::strong_ordering::greater;
		}

		if (other.m_prerelease.empty())
		{
			return std::strong_ordering::less;
		}

		const size_t shared_count = std::min(m_prerelease.size(), other.m_prerelease.size());
		for (size_t index = 0u; index < shared_count; index += 1u)
		{
			const std::strong_ordering identifier_order = CompareIdentifier(m_prerelease[index], other.m_prerelease[index]);
			if (identifier_order != 0)
			{
				return identifier_order;
			}
		}

		if (m_prerelease.size() < other.m_prerelease.size())
		{
			return std::strong_ordering::less;
		}
		if (m_prerelease.size() > other.m_prerelease.size())
		{
			return std::strong_ordering::greater;
		}
		return std::strong_ordering::equal;
	}

	bool SemanticVersion::operator==(const SemanticVersion& other) const noexcept
	{
		return (*this <=> other) == std::strong_ordering::equal;
	}

	bool VersionConstraint::Comparator::Matches(const SemanticVersion& version) const
	{
		switch (m_operator)
		{
		case Operator::Compatible:
		{
			const SemanticVersion upper_bound = IncrementCompatibleUpperBound(m_version);
			return version >= m_version && version < upper_bound;
		}
		case Operator::PatchCompatible:
		{
			const SemanticVersion upper_bound = IncrementPatchCompatibleUpperBound(m_version);
			return version >= m_version && version < upper_bound;
		}
		case Operator::Equal:
			return version == m_version;
		case Operator::Greater:
			return version > m_version;
		case Operator::GreaterEqual:
			return version >= m_version;
		case Operator::Less:
			return version < m_version;
		case Operator::LessEqual:
			return version <= m_version;
		}

		return false;
	}

	std::string VersionConstraint::Comparator::ToString() const
	{
		switch (m_operator)
		{
		case Operator::Compatible:
			return "^" + m_version.ToString();
		case Operator::PatchCompatible:
			return "~" + m_version.ToString();
		case Operator::Equal:
			return "=" + m_version.ToString();
		case Operator::Greater:
			return ">" + m_version.ToString();
		case Operator::GreaterEqual:
			return ">=" + m_version.ToString();
		case Operator::Less:
			return "<" + m_version.ToString();
		case Operator::LessEqual:
			return "<=" + m_version.ToString();
		}

		return m_version.ToString();
	}

	std::expected<VersionConstraint, std::string> VersionConstraint::Parse(std::string_view text)
	{
		VersionConstraint constraint;

		std::string trimmed = Trim(text);
		if (trimmed.empty())
		{
			return std::unexpected("Constraint string is empty.");
		}

		size_t start = 0u;
		while (start <= trimmed.size())
		{
			const size_t comma = trimmed.find(',', start);
			const std::string segment = Trim(std::string_view(trimmed).substr(start, comma == std::string::npos ? trimmed.size() - start : comma - start));
			if (segment.empty())
			{
				return std::unexpected(std::format("Invalid empty comparator in constraint '{}'.", trimmed));
			}

			Comparator comparator;
			std::string_view version_text(segment);
			if (segment.starts_with(">="))
			{
				comparator.m_operator = Operator::GreaterEqual;
				version_text.remove_prefix(2u);
			}
			else if (segment.starts_with("<="))
			{
				comparator.m_operator = Operator::LessEqual;
				version_text.remove_prefix(2u);
			}
			else if (segment.starts_with('^'))
			{
				comparator.m_operator = Operator::Compatible;
				version_text.remove_prefix(1u);
			}
			else if (segment.starts_with('~'))
			{
				comparator.m_operator = Operator::PatchCompatible;
				version_text.remove_prefix(1u);
			}
			else if (segment.starts_with('>'))
			{
				comparator.m_operator = Operator::Greater;
				version_text.remove_prefix(1u);
			}
			else if (segment.starts_with('<'))
			{
				comparator.m_operator = Operator::Less;
				version_text.remove_prefix(1u);
			}
			else if (segment.starts_with('='))
			{
				comparator.m_operator = Operator::Equal;
				version_text.remove_prefix(1u);
			}
			else
			{
				comparator.m_operator = Operator::Compatible;
			}

			const std::expected<SemanticVersion, std::string> version = SemanticVersion::Parse(version_text);
			if (!version.has_value())
			{
				return std::unexpected(std::format("Invalid constraint '{}': {}", segment, version.error()));
			}

			comparator.m_version = version.value();
			constraint.m_comparators.emplace_back(std::move(comparator));

			if (comma == std::string::npos)
			{
				break;
			}
			start = comma + 1u;
		}

		return constraint;
	}

	bool VersionConstraint::Matches(const SemanticVersion& version) const
	{
		return std::ranges::all_of(
			m_comparators,
			[&version](const Comparator& comparator)
			{
				return comparator.Matches(version);
			});
	}

	bool VersionConstraint::Empty() const noexcept
	{
		return m_comparators.empty();
	}

	std::string VersionConstraint::ToString() const
	{
		std::string result;
		for (size_t index = 0u; index < m_comparators.size(); index += 1u)
		{
			if (index != 0u)
			{
				result.append(", ");
			}
			result.append(m_comparators[index].ToString());
		}
		return result;
	}

	const std::vector<VersionConstraint::Comparator>& VersionConstraint::GetComparators() const noexcept
	{
		return m_comparators;
	}
}
