#include <algorithm>
#include <charconv>
#include <format>
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

void PatternCoverage::Add(const MidoriPattern& pattern)
{
	m_rows.push_back(Row{ Strip(&pattern) });
}

bool PatternCoverage::IsUseful(const MidoriPattern& pattern) const
{
	return Useful(m_rows, Row{ Strip(&pattern) });
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
			else if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
			{
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
		[&head, &pointers]<typename T>(const T& node) -> std::optional<Row>
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				return node.m_name == head.m_key ? std::optional<Row>(pointers(node.m_args)) : std::nullopt;
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
			else if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
			{
				return LiteralKey(node) == head.m_key ? std::optional<Row>(Row{}) : std::nullopt;
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
	}
	std::unreachable();
}
