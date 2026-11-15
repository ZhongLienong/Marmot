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
	PatternCoverage() = default;

	void Add(const MarmotPattern& pattern);

	// Whether the pattern matches some value no pattern added so far does.
	bool IsUseful(const MarmotPattern& pattern) const;

	bool IsExhaustive() const;

	// An alternative of an or-pattern, at any depth, that can never match: what
	// reaches it is taken by the patterns added so far or by an alternative tried
	// before it. Nothing when every alternative can match.
	const MarmotPattern* FindUnreachableAlternative(const MarmotPattern& pattern) const;

	// Values no pattern added so far matches, written as patterns, at most a few.
	std::vector<std::string> FindUnmatched() const;

private:
	using Row = std::vector<const MarmotPattern*>;
	using Witness = std::vector<std::string>;

	enum class Shape
	{
		Named,
		Tuple,
		ClosedArray,
		OpenArray,
		Literal,
		Interval
	};

	// One constructor of a column's type. An open array stands for every length
	// from its arity up: its first `m_before` arguments are counted from the start
	// and the rest from the end. An interval stands for the Ints `m_low` to `m_high`.
	struct Head
	{
		std::string m_key;
		Shape m_shape;
		size_t m_arity;
		size_t m_before;
		int64_t m_low = 0;
		int64_t m_high = 0;

		Head(std::string&& key, Shape shape, size_t arity, size_t before = 0uz);
		Head(int64_t low, int64_t high);
	};

	using Interval = std::pair<int64_t, int64_t>;

	// An or-pattern narrowed to its alternatives from `m_begin` up to `m_end`.
	struct Choice
	{
		const MarmotPattern* m_or;
		size_t m_begin;
		size_t m_end;

		Choice(const MarmotPattern* either, size_t begin, size_t end);
	};

	struct NestedOr
	{
		const MarmotPattern* m_or;
		// The alternative each or-pattern around it takes to reach it.
		std::vector<Choice> m_path;

		NestedOr(const MarmotPattern* either, std::vector<Choice>&& path);
	};

	std::vector<Row> m_rows;

	static std::vector<Witness> Unmatched(const std::vector<Row>& rows, size_t width);

	static bool Useful(const std::vector<Row>& rows, const Row& candidate);

	static Row Joined(Row&& first, std::span<const MarmotPattern* const> rest);

	static std::vector<Witness> Prefixed(const std::string& first, std::vector<Witness>&& rest);

	static std::vector<Row> Expanded(const std::vector<Row>& rows);

	static std::vector<Row> Specialize(const std::vector<Row>& rows, const Head& head);

	static std::vector<Row> Default(const std::vector<Row>& rows);

	static std::optional<std::vector<Head>> Signature(std::span<const MarmotPattern* const> column);

	static std::optional<Row> ArgumentsFor(const MarmotPattern& pattern, const Head& head);

	static Head OwnHead(const MarmotPattern& pattern);

	static std::optional<Interval> IntervalOf(const MarmotPattern& pattern);

	static std::optional<std::vector<Head>> Pieces(std::span<const MarmotPattern* const> column);

	static std::string LiteralKey(const MarmotPattern::Literal& literal);

	static const MarmotPattern* Strip(const MarmotPattern* pattern);

	static void CollectOrs(const MarmotPattern& pattern, std::vector<Choice>& path, std::vector<NestedOr>& found);

	static std::unique_ptr<MarmotPattern> Narrowed(const MarmotPattern& pattern, std::span<const Choice> choices);

	static bool IsWildcard(const MarmotPattern* pattern);

	static std::string Display(const Head& head, std::span<const std::string> arguments);
};
