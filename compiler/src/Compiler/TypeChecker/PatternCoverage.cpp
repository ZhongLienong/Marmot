#include <algorithm>
#include <format>
#include <ranges>
#include <type_traits>

#include "PatternCoverage.h"

namespace
{
	constexpr size_t s_max_unmatched = 3uz;
}

PatternCoverage::Head::Head(std::string&& key, Shape shape, size_t arity) : m_key(std::move(key)), m_shape(shape), m_arity(arity)
{
}

PatternCoverage::PatternCoverage(std::vector<const MidoriPattern*>&& patterns)
	: m_rows(patterns
		| std::views::transform([](const MidoriPattern* pattern) { return Row{ pattern }; })
		| std::ranges::to<std::vector>())
{
}

std::vector<std::string> PatternCoverage::FindUnmatched() const
{
	return Unmatched(m_rows, 1uz)
		| std::views::transform([](const Witness& witness) { return witness.front(); })
		| std::ranges::to<std::vector>();
}

// Each witness holds one value per column. Where the first column's type has a
// finite set of constructors and every one appears there, a value is unmatched
// only if it is unmatched under one of them; otherwise it can start with any
// constructor no row names, so only the rows that take anything there matter.
std::vector<PatternCoverage::Witness> PatternCoverage::Unmatched(const std::vector<Row>& rows, size_t width)
{
	if (rows.empty())
	{
		return { Witness(width, "_") };
	}
	if (width == 0uz)
	{
		return {};
	}

	std::vector<Head> present;
	for (const Row& row : rows)
	{
		if (!IsWildcard(row.front()))
		{
			Head head = HeadOf(*row.front());
			if (std::ranges::none_of(present, [&head](const Head& seen) { return seen.m_key == head.m_key; }))
			{
				present.emplace_back(std::move(head));
			}
		}
	}

	std::vector<Row>::const_iterator sample = std::ranges::find_if(rows, [](const Row& row) { return !IsWildcard(row.front()); });
	const std::optional<std::vector<Head>> signature = sample == rows.end() ? std::nullopt : Signature(*sample->front());
	const auto is_present = [&present](const Head& head)
		{
			return std::ranges::any_of(present, [&head](const Head& seen) { return seen.m_key == head.m_key; });
		};

	std::vector<Witness> unmatched;
	if (signature.has_value() && std::ranges::all_of(signature.value(), is_present))
	{
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

	const std::vector<Witness> rest = Unmatched(Default(rows), width - 1uz);
	if (rest.empty())
	{
		return {};
	}

	const std::vector<std::string> firsts = signature.has_value()
		? signature.value()
			| std::views::filter([&is_present](const Head& head) { return !is_present(head); })
			| std::views::transform([](const Head& head) { return Display(head, std::vector<std::string>(head.m_arity, "_")); })
			| std::ranges::to<std::vector>()
		: std::vector<std::string>{ "_" };

	for (const std::string& first : firsts)
	{
		for (const Witness& tail : rest)
		{
			Witness witness{ first };
			witness.insert(witness.end(), tail.begin(), tail.end());
			unmatched.emplace_back(std::move(witness));
			if (unmatched.size() == s_max_unmatched)
			{
				return unmatched;
			}
		}
	}
	return unmatched;
}

std::vector<PatternCoverage::Row> PatternCoverage::Specialize(const std::vector<Row>& rows, const Head& head)
{
	std::vector<Row> specialized;
	for (const Row& row : rows)
	{
		if (IsWildcard(row.front()))
		{
			Row expanded(head.m_arity, nullptr);
			expanded.insert(expanded.end(), row.begin() + 1, row.end());
			specialized.emplace_back(std::move(expanded));
		}
		else if (HeadOf(*row.front()).m_key == head.m_key)
		{
			Row expanded = ArgumentsOf(*row.front());
			expanded.insert(expanded.end(), row.begin() + 1, row.end());
			specialized.emplace_back(std::move(expanded));
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

// Every constructor of the sample's type, when there are finitely many to name.
std::optional<std::vector<PatternCoverage::Head>> PatternCoverage::Signature(const MidoriPattern& sample)
{
	return std::visit
	(
		[&sample]<typename T>(const T& node) -> std::optional<std::vector<Head>>
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				if (!node.m_is_union)
				{
					return std::vector<Head>{ HeadOf(sample) };
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
				return std::vector<Head>{ HeadOf(sample) };
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
			{
				if (node.m_kind == MidoriPattern::LiteralKind::Bool)
				{
					return std::vector<Head>{ Head("true", Shape::Literal, 0uz), Head("false", Shape::Literal, 0uz) };
				}
				if (node.m_kind == MidoriPattern::LiteralKind::Unit)
				{
					return std::vector<Head>{ HeadOf(sample) };
				}
				return std::nullopt;
			}
			else
			{
				return std::nullopt;
			}
		},
		*sample
	);
}

PatternCoverage::Head PatternCoverage::HeadOf(const MidoriPattern& pattern)
{
	return std::visit
	(
		[]<typename T>(const T& node) -> Head
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				return Head(std::string(node.m_name), Shape::Named, node.m_args.size());
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Tuple>)
			{
				return Head("()", Shape::Tuple, node.m_elements.size());
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Array>)
			{
				return Head(std::format("[{}]", node.m_elements.size()), Shape::Array, node.m_elements.size());
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
			{
				return Head(node.m_kind == MidoriPattern::LiteralKind::Unit ? std::string("()") : std::string(node.m_token.m_lexeme), Shape::Literal, 0uz);
			}
			else
			{
				static_assert(std::is_same_v<Node, MidoriPattern::Binding> || std::is_same_v<Node, MidoriPattern::Wildcard>);
				std::unreachable();
			}
		},
		*pattern
	);
}

PatternCoverage::Row PatternCoverage::ArgumentsOf(const MidoriPattern& pattern)
{
	const auto pointers = [](const std::vector<std::unique_ptr<MidoriPattern>>& patterns)
		{
			return patterns
				| std::views::transform([](const std::unique_ptr<MidoriPattern>& argument) -> const MidoriPattern* { return argument.get(); })
				| std::ranges::to<Row>();
		};

	return std::visit
	(
		[&pointers]<typename T>(const T& node) -> Row
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				return pointers(node.m_args);
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Tuple> || std::is_same_v<Node, MidoriPattern::Array>)
			{
				return pointers(node.m_elements);
			}
			else
			{
				return {};
			}
		},
		*pattern
	);
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
	case Shape::Array:
		return std::format("[{}]", joined);
	case Shape::Literal:
		return head.m_key;
	}
	std::unreachable();
}
