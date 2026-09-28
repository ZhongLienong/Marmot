#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <ranges>
#include <type_traits>

#include "PatternCoverage.h"

namespace
{
	constexpr size_t s_max_unmatched = 3uz;
}

PatternCoverage::Head::Head(std::string&& key, Shape shape, size_t arity, size_t before) : m_key(std::move(key)), m_shape(shape), m_arity(arity), m_before(before)
{
}

PatternCoverage::Head::Head(int64_t low, int64_t high) : m_key(std::format("{}..{}", low, high)), m_shape(Shape::Interval), m_arity(0uz), m_before(0uz), m_low(low), m_high(high)
{
}

PatternCoverage::Choice::Choice(const MidoriPattern* either, size_t begin, size_t end) : m_or(either), m_begin(begin), m_end(end)
{
}

PatternCoverage::NestedOr::NestedOr(const MidoriPattern* either, std::vector<Choice>&& path) : m_or(either), m_path(std::move(path))
{
}

void PatternCoverage::Add(const MidoriPattern& pattern)
{
	m_rows.push_back(Row{ Strip(&pattern) });
}

bool PatternCoverage::IsUseful(const MidoriPattern& pattern) const
{
	return Useful(m_rows, Row{ Strip(&pattern) });
}

// Alternative j of an or-pattern is tried on what reaches it: the values the
// case would match with that or-pattern cut down to j and every one around it to
// the branch that leads there. It can match when some such value is taken
// neither by an earlier case, nor by an earlier alternative of this or-pattern,
// nor by an earlier alternative of one around it.
const MidoriPattern* PatternCoverage::FindUnreachableAlternative(const MidoriPattern& pattern) const
{
	std::vector<Choice> path;
	std::vector<NestedOr> ors;
	CollectOrs(pattern, path, ors);

	for (const NestedOr& nested : ors)
	{
		const std::vector<std::unique_ptr<MidoriPattern>>& alternatives = nested.m_or->GetPattern<MidoriPattern::Or>().m_alternatives;
		for (size_t index = 0uz; index < alternatives.size(); index += 1uz)
		{
			std::vector<std::unique_ptr<MidoriPattern>> narrowed;
			std::vector<Row> rows = m_rows;
			const auto add_row = [&narrowed, &rows, &pattern](std::vector<Choice>&& choices)
				{
					narrowed.push_back(Narrowed(pattern, choices));
					rows.push_back(Row{ Strip(narrowed.back().get()) });
				};

			for (size_t depth = 0uz; depth < nested.m_path.size(); depth += 1uz)
			{
				const Choice& taken = nested.m_path[depth];
				if (taken.m_begin > 0uz)
				{
					std::vector<Choice> choices(nested.m_path.begin(), nested.m_path.begin() + static_cast<std::ptrdiff_t>(depth));
					choices.emplace_back(taken.m_or, 0uz, taken.m_begin);
					add_row(std::move(choices));
				}
			}
			if (index > 0uz)
			{
				std::vector<Choice> choices = nested.m_path;
				choices.emplace_back(nested.m_or, 0uz, index);
				add_row(std::move(choices));
			}

			std::vector<Choice> choices = nested.m_path;
			choices.emplace_back(nested.m_or, index, index + 1uz);
			const std::unique_ptr<MidoriPattern> candidate = Narrowed(pattern, choices);
			if (!Useful(rows, Row{ Strip(candidate.get()) }))
			{
				return alternatives[index].get();
			}
		}
	}
	return nullptr;
}

// Every or-pattern in the pattern, outer ones first, each with the branches of
// the ones around it that lead to it.
void PatternCoverage::CollectOrs(const MidoriPattern& pattern, std::vector<Choice>& path, std::vector<NestedOr>& found)
{
	const auto children = [&path, &found](std::span<const std::unique_ptr<MidoriPattern>> patterns)
		{
			for (const std::unique_ptr<MidoriPattern>& child : patterns)
			{
				CollectOrs(*child, path, found);
			}
		};

	std::visit
	(
		[&]<typename T>(const T& node)
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Or>)
			{
				found.emplace_back(&pattern, std::vector<Choice>(path));
				for (size_t index = 0uz; index < node.m_alternatives.size(); index += 1uz)
				{
					path.emplace_back(&pattern, index, index + 1uz);
					CollectOrs(*node.m_alternatives[index], path, found);
					path.pop_back();
				}
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Tuple>)
			{
				children(node.m_elements);
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Array>)
			{
				children(node.m_elements);
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				children(node.m_args);
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::As>)
			{
				CollectOrs(*node.m_pattern, path, found);
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Record>)
			{
				for (const MidoriPattern::Record::Field& field : node.m_fields)
				{
					CollectOrs(*field.m_pattern, path, found);
				}
			}
		},
		*pattern
	);
}

// A copy of the pattern with each or-pattern the choices name cut down to their
// alternatives. It keeps every type the checker recorded, which coverage reads.
std::unique_ptr<MidoriPattern> PatternCoverage::Narrowed(const MidoriPattern& pattern, std::span<const Choice> choices)
{
	const auto copies = [choices](std::span<const std::unique_ptr<MidoriPattern>> patterns)
		{
			return patterns
				| std::views::transform([choices](const std::unique_ptr<MidoriPattern>& child) { return Narrowed(*child, choices); })
				| std::ranges::to<std::vector>();
		};

	const std::span<const Choice>::iterator choice = std::ranges::find(choices, &pattern, &Choice::m_or);
	if (choice != choices.end())
	{
		const std::span<const std::unique_ptr<MidoriPattern>> kept = std::span(pattern.GetPattern<MidoriPattern::Or>().m_alternatives).subspan(choice->m_begin, choice->m_end - choice->m_begin);
		if (kept.size() == 1uz)
		{
			return Narrowed(*kept.front(), choices);
		}
		std::unique_ptr<MidoriPattern> either = std::make_unique<MidoriPattern>(MidoriPattern::Or(copies(kept)));
		either->GetType() = pattern.GetType();
		return either;
	}

	std::unique_ptr<MidoriPattern> copy = std::visit
	(
		[&copies, choices]<typename T>(const T& node) -> std::unique_ptr<MidoriPattern>
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Tuple>)
			{
				return std::make_unique<MidoriPattern>(MidoriPattern::Tuple(node.m_left_paren, copies(node.m_elements)));
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Array>)
			{
				std::optional<MidoriPattern::Array::Rest> rest;
				if (node.m_rest.has_value())
				{
					rest.emplace(node.m_rest->m_position, Narrowed(*node.m_rest->m_pattern, choices));
				}
				return std::make_unique<MidoriPattern>(MidoriPattern::Array(node.m_left_bracket, copies(node.m_elements), std::move(rest)));
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				MidoriPattern::Constructor constructor(node.m_name_token, std::string(node.m_name), copies(node.m_args), node.m_is_union);
				constructor.m_tag = node.m_tag;
				return std::make_unique<MidoriPattern>(std::move(constructor));
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::As>)
			{
				return std::make_unique<MidoriPattern>(MidoriPattern::As(Narrowed(*node.m_pattern, choices), Narrowed(*node.m_binding, choices)));
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Or>)
			{
				return std::make_unique<MidoriPattern>(MidoriPattern::Or(copies(node.m_alternatives)));
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Record>)
			{
				std::vector<MidoriPattern::Record::Field> fields;
				for (const MidoriPattern::Record::Field& field : node.m_fields)
				{
					fields.emplace_back(field.m_name, Narrowed(*field.m_pattern, choices));
					fields.back().m_index = field.m_index;
				}
				return std::make_unique<MidoriPattern>(MidoriPattern::Record(node.m_name_token, std::string(node.m_name), std::move(fields), std::optional<Token>(node.m_rest)));
			}
			else
			{
				return std::make_unique<MidoriPattern>(Node(node));
			}
		},
		*pattern
	);
	copy->GetType() = pattern.GetType();
	return copy;
}

bool PatternCoverage::IsExhaustive() const
{
	return !Useful(m_rows, Row{ nullptr });
}

std::vector<std::string> PatternCoverage::FindUnmatched() const
{
	return Unmatched(m_rows, 1uz)
		| std::views::transform([](const Witness& witness) { return witness.front(); })
		| std::ranges::to<std::vector>();
}

// Each witness holds one value per column. When the first column's type has
// finitely many constructors, a value is unmatched exactly when it is unmatched
// under one of them. Otherwise no set of constructors covers the column, so only
// the rows that take anything there can match, and the value starts with `_`.
std::vector<PatternCoverage::Witness> PatternCoverage::Unmatched(const std::vector<Row>& unexpanded, size_t width)
{
	if (unexpanded.empty())
	{
		return { Witness(width, "_") };
	}
	if (width == 0uz)
	{
		return {};
	}

	const std::vector<Row> rows = Expanded(unexpanded);

	const std::vector<const MidoriPattern*> column = rows
		| std::views::transform([](const Row& row) { return row.front(); })
		| std::views::filter([](const MidoriPattern* pattern) { return !IsWildcard(pattern); })
		| std::ranges::to<std::vector>();
	const std::optional<std::vector<Head>> signature = column.empty() ? std::nullopt : Signature(column);
	if (!signature.has_value())
	{
		return Prefixed("_", Unmatched(Default(rows), width - 1uz));
	}

	std::vector<Witness> unmatched;
	for (const Head& head : signature.value())
	{
		for (const Witness& inner : Unmatched(Specialize(rows, head), head.m_arity + width - 1uz))
		{
			Witness witness{ Display(head, std::span(inner).first(head.m_arity)) };
			witness.insert(witness.end(), inner.begin() + static_cast<std::ptrdiff_t>(head.m_arity), inner.end());
			unmatched.emplace_back(std::move(witness));
			if (unmatched.size() == s_max_unmatched)
			{
				return unmatched;
			}
		}
	}
	return unmatched;
}

// Maranget's usefulness check: whether some value the candidate row matches is
// matched by no row. The candidate's own head joins the column when choosing the
// constructors to split on, since an array with a rest there changes them.
bool PatternCoverage::Useful(const std::vector<Row>& unexpanded, const Row& candidate)
{
	if (unexpanded.empty())
	{
		return true;
	}
	if (candidate.empty())
	{
		return false;
	}

	const MidoriPattern* head = candidate.front();
	const std::span<const MidoriPattern* const> rest = std::span(candidate).subspan(1uz);
	if (!IsWildcard(head) && head->IsPattern<MidoriPattern::Or>())
	{
		return std::ranges::any_of(head->GetPattern<MidoriPattern::Or>().m_alternatives, [&unexpanded, rest](const std::unique_ptr<MidoriPattern>& alternative)
			{
				return Useful(unexpanded, Joined(Row{ Strip(alternative.get()) }, rest));
			});
	}

	const std::vector<Row> rows = Expanded(unexpanded);
	std::vector<const MidoriPattern*> column = rows
		| std::views::transform([](const Row& row) { return row.front(); })
		| std::views::filter([](const MidoriPattern* pattern) { return !IsWildcard(pattern); })
		| std::ranges::to<std::vector>();
	if (!IsWildcard(head))
	{
		column.push_back(head);
	}

	const std::optional<std::vector<Head>> signature = column.empty() ? std::nullopt : Signature(column);
	if (signature.has_value())
	{
		return std::ranges::any_of(signature.value(), [&rows, head, rest](const Head& constructor)
			{
				std::optional<Row> arguments = IsWildcard(head) ? Row(constructor.m_arity, nullptr) : ArgumentsFor(*head, constructor);
				return arguments.has_value() && Useful(Specialize(rows, constructor), Joined(std::move(arguments.value()), rest));
			});
	}
	if (IsWildcard(head))
	{
		return Useful(Default(rows), Row(rest.begin(), rest.end()));
	}

	const Head own = OwnHead(*head);
	return Useful(Specialize(rows, own), Joined(ArgumentsFor(*head, own).value(), rest));
}

// Every Int, split where an interval the column names starts or stops, so that
// each piece lies wholly inside or wholly outside every one of them.
std::optional<std::vector<PatternCoverage::Head>> PatternCoverage::Pieces(std::span<const MidoriPattern* const> column)
{
	constexpr int64_t low = std::numeric_limits<int64_t>::min();
	constexpr int64_t high = std::numeric_limits<int64_t>::max();
	std::vector<int64_t> starts{ low };
	for (const MidoriPattern* pattern : column)
	{
		const std::optional<Interval> other = IntervalOf(*pattern);
		if (!other.has_value())
		{
			return std::nullopt;
		}
		if (other->first > low)
		{
			starts.push_back(other->first);
		}
		if (other->second < high)
		{
			starts.push_back(other->second + 1);
		}
	}
	std::ranges::sort(starts);
	const auto [unique_end, sentinel] = std::ranges::unique(starts);
	starts.erase(unique_end, sentinel);

	std::vector<Head> pieces;
	for (size_t index = 0uz; index < starts.size(); index += 1uz)
	{
		pieces.emplace_back(starts[index], index + 1uz < starts.size() ? starts[index + 1uz] - 1 : high);
	}
	return pieces;
}

// Int literals and ranges, which are the patterns an Int column tells apart by
// value. A literal too large for an Int has none; lowering reports it.
std::optional<PatternCoverage::Interval> PatternCoverage::IntervalOf(const MidoriPattern& pattern)
{
	if (pattern.IsPattern<MidoriPattern::Range>())
	{
		const MidoriPattern::Range& range = pattern.GetPattern<MidoriPattern::Range>();
		return Interval{ range.m_low, range.m_high };
	}
	if (pattern.IsPattern<MidoriPattern::Literal>() && pattern.GetPattern<MidoriPattern::Literal>().m_kind == MidoriPattern::LiteralKind::Integer)
	{
		return ParseIntegerLiteral(pattern.GetPattern<MidoriPattern::Literal>().m_token.m_lexeme)
			.transform([](int64_t value) { return Interval{ value, value }; });
	}
	return std::nullopt;
}

PatternCoverage::Row PatternCoverage::Joined(Row&& first, std::span<const MidoriPattern* const> rest)
{
	first.insert(first.end(), rest.begin(), rest.end());
	return std::move(first);
}

std::vector<PatternCoverage::Witness> PatternCoverage::Prefixed(const std::string& first, std::vector<Witness>&& rest)
{
	for (Witness& witness : rest)
	{
		witness.insert(witness.begin(), first);
	}
	return std::move(rest);
}

// A row that starts with an or-pattern stands for one row per alternative.
std::vector<PatternCoverage::Row> PatternCoverage::Expanded(const std::vector<Row>& rows)
{
	std::vector<Row> expanded;
	for (const Row& row : rows)
	{
		if (IsWildcard(row.front()) || !row.front()->IsPattern<MidoriPattern::Or>())
		{
			expanded.push_back(row);
			continue;
		}

		const std::vector<Row> alternatives = row.front()->GetPattern<MidoriPattern::Or>().m_alternatives
			| std::views::transform([&row](const std::unique_ptr<MidoriPattern>& alternative)
				{
					Row alternative_row{ Strip(alternative.get()) };
					alternative_row.insert(alternative_row.end(), row.begin() + 1, row.end());
					return alternative_row;
				})
			| std::ranges::to<std::vector>();
		const std::vector<Row> flattened = Expanded(alternatives);
		expanded.insert(expanded.end(), flattened.begin(), flattened.end());
	}
	return expanded;
}

std::vector<PatternCoverage::Row> PatternCoverage::Specialize(const std::vector<Row>& rows, const Head& head)
{
	std::vector<Row> specialized;
	for (const Row& row : rows)
	{
		std::optional<Row> expanded = IsWildcard(row.front()) ? Row(head.m_arity, nullptr) : ArgumentsFor(*row.front(), head);
		if (expanded.has_value())
		{
			expanded->insert(expanded->end(), row.begin() + 1, row.end());
			specialized.emplace_back(std::move(expanded.value()));
		}
	}
	return specialized;
}

std::vector<PatternCoverage::Row> PatternCoverage::Default(const std::vector<Row>& rows)
{
	return rows
		| std::views::filter([](const Row& row) { return IsWildcard(row.front()); })
		| std::views::transform([](const Row& row) { return row | std::views::drop(1) | std::ranges::to<Row>(); })
		| std::ranges::to<std::vector>();
}

// Every constructor of the column's type, when there are finitely many to name.
// Arrays have one per length, which is finite only when a rest pattern is there.
// From the longest prefix plus the longest suffix any rest pattern has, no
// pattern's prefix and suffix meet, so every longer length is matched alike and
// stands as one open head; it also starts past every length written without a rest.
std::optional<std::vector<PatternCoverage::Head>> PatternCoverage::Signature(std::span<const MidoriPattern* const> column)
{
	return std::visit
	(
		[column]<typename T>(const T& node) -> std::optional<std::vector<Head>>
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				if (!node.m_is_union)
				{
					return std::vector<Head>{ Head(std::string(node.m_name), Shape::Named, node.m_args.size()) };
				}

				const MidoriType::UnionType& union_type = node.m_type_data->template GetType<MidoriType::UnionType>();
				std::vector<std::pair<int, Head>> members = union_type.m_member_info
					| std::views::transform([](const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& member)
						{
							return std::pair<int, Head>{ member.second.m_tag, Head(std::string(member.first), Shape::Named, member.second.m_member_types.size()) };
						})
					| std::ranges::to<std::vector>();
				std::ranges::sort(members, {}, &std::pair<int, Head>::first);
				return members | std::views::values | std::ranges::to<std::vector>();
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Record>)
			{
				return std::vector<Head>{ Head(std::string(node.m_name), Shape::Named, node.m_type_data->template GetType<MidoriType::StructType>().m_member_types.size()) };
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Tuple>)
			{
				return std::vector<Head>{ Head("()", Shape::Tuple, node.m_elements.size()) };
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Array>)
			{
				const std::vector<const MidoriPattern::Array*> arrays = column
					| std::views::transform([](const MidoriPattern* pattern) { return &pattern->GetPattern<MidoriPattern::Array>(); })
					| std::ranges::to<std::vector>();
				if (std::ranges::none_of(arrays, [](const MidoriPattern::Array* array) { return array->m_rest.has_value(); }))
				{
					return std::nullopt;
				}

				size_t before = 0uz;
				size_t after = 0uz;
				size_t past_closed = 0uz;
				for (const MidoriPattern::Array* array : arrays)
				{
					if (array->m_rest.has_value())
					{
						before = std::max(before, array->m_rest->m_position);
						after = std::max(after, array->m_elements.size() - array->m_rest->m_position);
					}
					else
					{
						past_closed = std::max(past_closed, array->m_elements.size() + 1uz);
					}
				}
				before = std::max(before, past_closed - std::min(past_closed, after));

				const size_t open_from = before + after;
				std::vector<Head> lengths = std::views::iota(0uz, open_from)
					| std::views::transform([](size_t length) { return Head(std::format("[{}]", length), Shape::ClosedArray, length); })
					| std::ranges::to<std::vector>();
				lengths.emplace_back(std::format("[{}..]", open_from), Shape::OpenArray, open_from, before);
				return lengths;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Range>)
			{
				return Pieces(column);
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
			{
				if (node.m_kind == MidoriPattern::LiteralKind::Integer)
				{
					return Pieces(column);
				}
				if (node.m_kind == MidoriPattern::LiteralKind::Bool)
				{
					return std::vector<Head>{ Head("true", Shape::Literal, 0uz), Head("false", Shape::Literal, 0uz) };
				}
				if (node.m_kind == MidoriPattern::LiteralKind::Unit)
				{
					return std::vector<Head>{ Head("()", Shape::Literal, 0uz) };
				}
				return std::nullopt;
			}
			else
			{
				return std::nullopt;
			}
		},
		**column.front()
	);
}

// The patterns for a head's arguments when `pattern` matches values that start
// with it. An array with a rest matches every length it has room for, with
// wildcards where the rest is.
std::optional<PatternCoverage::Row> PatternCoverage::ArgumentsFor(const MidoriPattern& pattern, const Head& head)
{
	const auto pointers = [](std::span<const std::unique_ptr<MidoriPattern>> patterns)
		{
			return patterns
				| std::views::transform([](const std::unique_ptr<MidoriPattern>& argument) { return Strip(argument.get()); })
				| std::ranges::to<Row>();
		};

	return std::visit
	(
		[&pattern, &head, &pointers]<typename T>(const T& node) -> std::optional<Row>
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				return node.m_name == head.m_key ? std::optional<Row>(pointers(node.m_args)) : std::nullopt;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Record>)
			{
				if (node.m_name != head.m_key)
				{
					return std::nullopt;
				}
				Row arguments(head.m_arity, nullptr);
				for (const MidoriPattern::Record::Field& field : node.m_fields)
				{
					arguments[static_cast<size_t>(field.m_index)] = Strip(field.m_pattern.get());
				}
				return arguments;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Tuple>)
			{
				return pointers(node.m_elements);
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Array>)
			{
				if (!node.m_rest.has_value())
				{
					return head.m_shape == Shape::ClosedArray && node.m_elements.size() == head.m_arity ? std::optional<Row>(pointers(node.m_elements)) : std::nullopt;
				}
				if (node.m_elements.size() > head.m_arity)
				{
					return std::nullopt;
				}

				const std::span<const std::unique_ptr<MidoriPattern>> elements(node.m_elements);
				Row arguments = pointers(elements.first(node.m_rest->m_position));
				arguments.insert(arguments.end(), head.m_arity - elements.size(), nullptr);
				const Row after = pointers(elements.subspan(node.m_rest->m_position));
				arguments.insert(arguments.end(), after.begin(), after.end());
				return arguments;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Literal> || std::is_same_v<Node, MidoriPattern::Range>)
			{
				if (head.m_shape == Shape::Interval)
				{
					const std::optional<Interval> interval = IntervalOf(pattern);
					const bool contains = interval.has_value() && interval->first <= head.m_low && head.m_high <= interval->second;
					return contains ? std::optional<Row>(Row{}) : std::nullopt;
				}
				if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
				{
					return LiteralKey(node) == head.m_key ? std::optional<Row>(Row{}) : std::nullopt;
				}
				return std::nullopt;
			}
			else
			{
				static_assert(std::is_same_v<Node, MidoriPattern::Binding> || std::is_same_v<Node, MidoriPattern::Wildcard> || std::is_same_v<Node, MidoriPattern::As> || std::is_same_v<Node, MidoriPattern::Or>);
				std::unreachable();
			}
		},
		*pattern
	);
}

// The one constructor a pattern names in a column with no finite signature: a
// literal, or an array of one exact length.
PatternCoverage::Head PatternCoverage::OwnHead(const MidoriPattern& pattern)
{
	if (pattern.IsPattern<MidoriPattern::Array>())
	{
		const size_t length = pattern.GetPattern<MidoriPattern::Array>().m_elements.size();
		return Head(std::format("[{}]", length), Shape::ClosedArray, length);
	}
	return Head(LiteralKey(pattern.GetPattern<MidoriPattern::Literal>()), Shape::Literal, 0uz);
}

// Literals that denote one value share a key however they are written, so `16`
// and `0x10` are the same case.
std::string PatternCoverage::LiteralKey(const MidoriPattern::Literal& literal)
{
	const std::string_view lexeme = literal.m_token.m_lexeme;
	switch (literal.m_kind)
	{
	case MidoriPattern::LiteralKind::Unit:
		return "()";
	case MidoriPattern::LiteralKind::Integer:
	case MidoriPattern::LiteralKind::Byte:
	case MidoriPattern::LiteralKind::Word:
	{
		// A literal too large for 64 bits is reported by lowering; until then its
		// lexeme stands for it.
		const bool is_negative = lexeme.starts_with('-');
		return ParseUnsignedLiteral(is_negative ? lexeme.substr(1uz) : lexeme)
			.transform([is_negative](uint64_t magnitude) { return std::format("{}{}", is_negative && magnitude != 0u ? "-" : "", magnitude); })
			.value_or(std::string(lexeme));
	}
	case MidoriPattern::LiteralKind::Float:
	{
		double value = 0.0;
		std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
		return std::format("{}", value);
	}
	case MidoriPattern::LiteralKind::Bool:
	case MidoriPattern::LiteralKind::Text:
		return std::string(lexeme);
	}
	std::unreachable();
}

// Rows never hold an `as`: it matches what its pattern does.
const MidoriPattern* PatternCoverage::Strip(const MidoriPattern* pattern)
{
	return pattern->IsPattern<MidoriPattern::As>() ? Strip(pattern->GetPattern<MidoriPattern::As>().m_pattern.get()) : pattern;
}

bool PatternCoverage::IsWildcard(const MidoriPattern* pattern)
{
	return pattern == nullptr || pattern->IsPattern<MidoriPattern::Binding>() || pattern->IsPattern<MidoriPattern::Wildcard>();
}

std::string PatternCoverage::Display(const Head& head, std::span<const std::string> arguments)
{
	const std::string joined = arguments | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
	switch (head.m_shape)
	{
	case Shape::Named:
		return std::format("{}({})", head.m_key, joined);
	case Shape::Tuple:
		return std::format("({})", joined);
	case Shape::ClosedArray:
		return std::format("[{}]", joined);
	case Shape::OpenArray:
	{
		std::vector<std::string> parts(arguments.begin(), arguments.end());
		parts.insert(parts.begin() + static_cast<std::ptrdiff_t>(head.m_before), "..");
		return std::format("[{}]", parts | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>());
	}
	case Shape::Literal:
		return head.m_key;
	case Shape::Interval:
		// One value stands for the piece: the one nearest zero, which is next to a
		// case's interval whenever the piece does not hold zero.
		return std::format("{}", head.m_high < 0 ? head.m_high : std::max<int64_t>(head.m_low, 0));
	}
	std::unreachable();
}
