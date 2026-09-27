#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

// Finds values that no pattern of a match takes, by Maranget's usefulness check
// over the matrix of patterns, so coverage is decided at every depth rather than
// by the outermost constructor alone. Lowering relies on it: nothing follows a
// match's last case.
class PatternCoverage
{
public:
	explicit PatternCoverage(std::vector<const MidoriPattern*>&& patterns);

	// Values no pattern matches, written as patterns, at most a few of them.
	std::vector<std::string> FindUnmatched() const;

private:
	using Row = std::vector<const MidoriPattern*>;
	using Witness = std::vector<std::string>;

	enum class Shape
	{
		Named,
		Tuple,
		Array,
		Literal
	};

	struct Head
	{
		std::string m_key;
		Shape m_shape;
		size_t m_arity;

		Head(std::string&& key, Shape shape, size_t arity);
	};

	std::vector<Row> m_rows;

	static std::vector<Witness> Unmatched(const std::vector<Row>& rows, size_t width);

	static std::vector<Row> Specialize(const std::vector<Row>& rows, const Head& head);

	static std::vector<Row> Default(const std::vector<Row>& rows);

	static std::optional<std::vector<Head>> Signature(const MidoriPattern& sample);

	static Head HeadOf(const MidoriPattern& pattern);

	static Row ArgumentsOf(const MidoriPattern& pattern);

	static bool IsWildcard(const MidoriPattern* pattern);

	static std::string Display(const Head& head, std::span<const std::string> arguments);
};
