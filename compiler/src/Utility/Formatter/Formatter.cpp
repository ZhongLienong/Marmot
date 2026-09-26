#include "Utility/Formatter/Formatter.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include "Common/Source/Source.h"
#include "Compiler/Lexer/Lexer.h"

namespace
{
	using TokenName = Token::Name;

	enum class TopLevelCategory
	{
		None,
		Module,
		ImportLike,
		Declaration
	};

	enum class ContextKind
	{
		BlockBrace,
		InlineBrace
	};

	struct Context
	{
		ContextKind m_kind = ContextKind::BlockBrace;
		int m_paren_depth = 0;
		int m_bracket_depth = 0;
		int m_block_depth = 0;
		// The indentation the brace's `}` is written at, and the one restored after it.
		// They differ for a match arm's body, which aligns with its `case`.
		int m_outer_indent = 0;
		int m_restore_indent = 0;
	};

	struct MatchContext
	{
		int m_indent = 0;
		int m_paren_depth = 0;
		int m_bracket_depth = 0;
		int m_block_depth = 0;
	};

	[[nodiscard]] bool IsComment(TokenName token_name)
	{
		return token_name == TokenName::LINE_COMMENT || token_name == TokenName::BLOCK_COMMENT;
	}

	[[nodiscard]] int CountEmbeddedNewlines(std::string_view text)
	{
		return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
	}

	[[nodiscard]] int EndSourceLine(const Token& token)
	{
		return token.m_line + CountEmbeddedNewlines(token.m_lexeme);
	}

	[[nodiscard]] const Token* PreviousNonCommentToken(const std::vector<Token>& tokens, size_t index)
	{
		if (index == 0u)
		{
			return nullptr;
		}

		for (size_t previous_index = index; previous_index > 0u; previous_index -= 1u)
		{
			const Token& token = tokens[previous_index - 1u];
			if (!IsComment(token.m_token_name))
			{
				return &token;
			}
		}

		return nullptr;
	}

	[[nodiscard]] const Token* NextNonCommentToken(const std::vector<Token>& tokens, size_t index)
	{
		for (size_t next_index = index + 1u; next_index < tokens.size(); next_index += 1u)
		{
			const Token& token = tokens[next_index];
			if (!IsComment(token.m_token_name))
			{
				return &token;
			}
		}

		return nullptr;
	}

	[[nodiscard]] std::string QuoteTextLiteral(std::string_view value)
	{
		std::string quoted;
		quoted.push_back('"');
		for (const char ch : value)
		{
			switch (ch)
			{
			case '\\':
				quoted += "\\\\";
				break;
			case '"':
				quoted += "\\\"";
				break;
			case '\n':
				quoted += "\\n";
				break;
			case '\r':
				quoted += "\\r";
				break;
			case '\t':
				quoted += "\\t";
				break;
			case '\b':
				quoted += "\\b";
				break;
			case '\f':
				quoted += "\\f";
				break;
			default:
				quoted.push_back(ch);
				break;
			}
		}
		quoted.push_back('"');
		return quoted;
	}

	[[nodiscard]] std::string TokenText(const Token& token)
	{
		if (token.m_token_name == TokenName::TEXT_LITERAL)
		{
			return QuoteTextLiteral(token.m_lexeme);
		}

		return token.m_lexeme;
	}

	[[nodiscard]] bool IsTopLevelStart(TokenName token_name)
	{
		switch (token_name)
		{
		case TokenName::MODULE:
		case TokenName::IMPORT:
		case TokenName::USE:
		case TokenName::PUBLIC:
		case TokenName::PRIVATE:
		case TokenName::DEF:
		case TokenName::CLASS:
		case TokenName::INSTANCE:
		case TokenName::TYPE:
		case TokenName::ALIAS:
		case TokenName::FOREIGN:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] TopLevelCategory ClassifyTopLevel(TokenName token_name)
	{
		switch (token_name)
		{
		case TokenName::MODULE:
			return TopLevelCategory::Module;
		case TokenName::IMPORT:
		case TokenName::USE:
		case TokenName::PUBLIC:
		case TokenName::PRIVATE:
			return TopLevelCategory::ImportLike;
		default:
			return TopLevelCategory::Declaration;
		}
	}

	[[nodiscard]] bool IsOperator(TokenName token_name)
	{
		switch (token_name)
		{
		case TokenName::THIN_ARROW:
		case TokenName::FAT_ARROW:
		case TokenName::SINGLE_PLUS:
		case TokenName::DOUBLE_PLUS:
		case TokenName::SINGLE_MINUS:
		case TokenName::LEFT_SHIFT:
		case TokenName::RIGHT_SHIFT:
		case TokenName::PERCENT:
		case TokenName::STAR:
		case TokenName::SLASH:
		case TokenName::SINGLE_BAR:
		case TokenName::DOUBLE_BAR:
		case TokenName::BAR_BRACKET:
		case TokenName::CARET:
		case TokenName::SINGLE_AMPERSAND:
		case TokenName::DOUBLE_AMPERSAND:
		case TokenName::BANG:
		case TokenName::BANG_EQUAL:
		case TokenName::SINGLE_EQUAL:
		case TokenName::DOUBLE_EQUAL:
		case TokenName::RIGHT_ANGLE:
		case TokenName::GREATER_EQUAL:
		case TokenName::LEFT_ANGLE:
		case TokenName::LESS_EQUAL:
		case TokenName::AS:
		case TokenName::IN:
		case TokenName::THEN:
		case TokenName::ELSE:
		case TokenName::WITH:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] bool IsUnaryPrefix(TokenName token_name)
	{
		switch (token_name)
		{
		case TokenName::BANG:
		case TokenName::HASH:
		case TokenName::SINGLE_MINUS:
		case TokenName::SINGLE_PLUS:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] bool IsWordLike(TokenName token_name)
	{
		switch (token_name)
		{
		case TokenName::IDENTIFIER_LITERAL:
		case TokenName::TEXT_LITERAL:
		case TokenName::FLOAT_LITERAL:
		case TokenName::INTEGER_LITERAL:
		case TokenName::ELSE:
		case TokenName::FALSE:
		case TokenName::FUNCTION:
		case TokenName::FOR:
		case TokenName::IN:
		case TokenName::IF:
		case TokenName::TRUE:
		case TokenName::DEF:
		case TokenName::IMPORT:
		case TokenName::AS:
		case TokenName::FOREIGN:
		case TokenName::CASE:
		case TokenName::MATCH:
		case TokenName::THEN:
		case TokenName::WITH:
		case TokenName::MODULE:
		case TokenName::EXPORT:
		case TokenName::PUBLIC:
		case TokenName::PRIVATE:
		case TokenName::USE:
		case TokenName::CLASS:
		case TokenName::INSTANCE:
		case TokenName::WHERE:
		case TokenName::TYPE:
		case TokenName::ALIAS:
		case TokenName::DERIVING:
		case TokenName::FLOAT:
		case TokenName::INTEGER:
		case TokenName::BYTE:
		case TokenName::WORD:
		case TokenName::TEXT:
		case TokenName::BOOL:
		case TokenName::UNIT:
		case TokenName::ARRAY:
		case TokenName::NEVER:
			return true;
		default:
			return false;
		}
	}

	// Whether `<` at `index` opens a type's arguments - `Array<Text>`, `fn<T>`,
	// `Map::Map<K, V>` - rather than comparing. It does when it follows a type-like
	// name and closes before anything a type cannot hold.
	[[nodiscard]] bool IsTypeLikeName(const Token& token)
	{
		switch (token.m_token_name)
		{
		case TokenName::FUNCTION:
		case TokenName::ARRAY:
		case TokenName::FLOAT:
		case TokenName::INTEGER:
		case TokenName::BYTE:
		case TokenName::WORD:
		case TokenName::TEXT:
		case TokenName::BOOL:
		case TokenName::UNIT:
		case TokenName::NEVER:
			return true;
		case TokenName::IDENTIFIER_LITERAL:
			return !token.m_lexeme.empty() && std::isupper(static_cast<unsigned char>(token.m_lexeme[0u])) != 0;
		default:
			return false;
		}
	}

	[[nodiscard]] bool CanAppearInTypeArguments(const Token& token)
	{
		switch (token.m_token_name)
		{
		case TokenName::IDENTIFIER_LITERAL:
		case TokenName::COMMA:
		case TokenName::DOUBLE_COLON:
		case TokenName::SINGLE_DOT:
		case TokenName::LEFT_PAREN:
		case TokenName::RIGHT_PAREN:
		case TokenName::THIN_ARROW:
		case TokenName::LEFT_ANGLE:
		case TokenName::RIGHT_ANGLE:
		case TokenName::RIGHT_SHIFT:
			return true;
		default:
			return IsTypeLikeName(token);
		}
	}

	// The indices of the `<` and closing `>` (or `>>`) tokens that bracket type arguments.
	[[nodiscard]] std::unordered_set<size_t> FindTypeArgumentBrackets(const std::vector<Token>& tokens)
	{
		std::unordered_set<size_t> brackets;
		for (size_t open = 0u; open < tokens.size(); open += 1u)
		{
			if (tokens[open].m_token_name != TokenName::LEFT_ANGLE || brackets.contains(open))
			{
				continue;
			}

			const Token* previous = PreviousNonCommentToken(tokens, open);
			if (previous == nullptr)
			{
				continue;
			}

			// `import { <IO>, <Math.Vector> }`: nothing else puts `<` after `{` or `,`.
			if (previous->m_token_name == TokenName::LEFT_BRACE || previous->m_token_name == TokenName::COMMA)
			{
				size_t close = open + 1u;
				while (close < tokens.size() && (tokens[close].m_token_name == TokenName::IDENTIFIER_LITERAL || tokens[close].m_token_name == TokenName::SINGLE_DOT))
				{
					close += 1u;
				}
				if (close > open + 1u && close < tokens.size() && tokens[close].m_token_name == TokenName::RIGHT_ANGLE)
				{
					brackets.insert(open);
					brackets.insert(close);
				}
				continue;
			}

			if (!IsTypeLikeName(*previous))
			{
				continue;
			}

			std::vector<size_t> opened{ open };
			std::vector<size_t> found{ open };
			for (size_t index = open + 1u; index < tokens.size() && !opened.empty(); index += 1u)
			{
				const Token& token = tokens[index];
				if (IsComment(token.m_token_name))
				{
					continue;
				}
				if (!CanAppearInTypeArguments(token))
				{
					break;
				}
				if (token.m_token_name == TokenName::LEFT_ANGLE)
				{
					opened.push_back(index);
					found.push_back(index);
				}
				else if (token.m_token_name == TokenName::RIGHT_ANGLE || token.m_token_name == TokenName::RIGHT_SHIFT)
				{
					const size_t closes = token.m_token_name == TokenName::RIGHT_SHIFT ? 2u : 1u;
					if (closes > opened.size())
					{
						break;
					}
					opened.resize(opened.size() - closes);
					found.push_back(index);
				}
			}

			if (opened.empty())
			{
				brackets.insert(found.begin(), found.end());
			}
		}
		return brackets;
	}

	[[nodiscard]] bool IsInlineBraceOpen(const std::vector<Token>& tokens, size_t index)
	{
		const Token* previous = PreviousNonCommentToken(tokens, index);
		if (previous == nullptr)
		{
			return false;
		}

		return previous->m_token_name == TokenName::IMPORT || previous->m_token_name == TokenName::EXPORT;
	}

	class FormatterEngine
	{
	private:
		std::string m_output;
		int m_indent = 0;
		int m_paren_depth = 0;
		int m_bracket_depth = 0;
		int m_block_depth = 0;
		bool m_at_line_start = true;
		TopLevelCategory m_last_top_level_category = TopLevelCategory::None;
		std::optional<TokenName> m_previous_token;
		// Whether the token written last was a unary prefix or a type-argument bracket,
		// which are spaced unlike the same token as an infix operator.
		bool m_previous_was_unary = false;
		// The last token that was code, which a comment does not reset: whether an
		// operator is unary depends on it even across `1 /* note */ + 2`.
		std::optional<TokenName> m_last_code_token;
		bool m_previous_was_type_bracket = false;
		std::unordered_set<size_t> m_type_brackets;
		// Type-argument brackets open around the current token: a comma inside them
		// separates type arguments, never record fields.
		int m_type_bracket_depth = 0;
		std::vector<Context> m_contexts;
		std::vector<MatchContext> m_match_contexts;

	public:
		[[nodiscard]] std::string Format(const std::vector<Token>& tokens)
		{
			m_type_brackets = FindTypeArgumentBrackets(tokens);
			for (size_t index = 0u; index < tokens.size(); index += 1u)
			{
				FormatToken(tokens, index);
			}

			if (!m_output.empty() && m_output.back() != '\n')
			{
				m_output.push_back('\n');
			}

			return m_output;
		}

	private:
		void WriteIndent(int indent)
		{
			if (!m_at_line_start)
			{
				return;
			}

			m_output.append(static_cast<size_t>(indent * 4), ' ');
		}

		void WriteCurrentIndent()
		{
			WriteIndent(m_indent);
		}

		void WriteNewline(int count = 1)
		{
			while (!m_output.empty() && m_output.back() == ' ')
			{
				m_output.pop_back();
			}

			for (int index = 0; index < count; index += 1)
			{
				if (m_output.empty() || m_output.back() != '\n')
				{
					m_output.push_back('\n');
				}
				else if (index + 1 < count)
				{
					m_output.push_back('\n');
				}
			}

			m_at_line_start = true;
		}

		// Ends the output with exactly `count` newlines, so `2` leaves one blank line.
		void EnsureTrailingNewlines(int count)
		{
			while (!m_output.empty() && m_output.back() == ' ')
			{
				m_output.pop_back();
			}

			int trailing = 0;
			for (std::string::const_reverse_iterator it = m_output.crbegin(); it != m_output.crend() && *it == '\n'; ++it)
			{
				trailing += 1;
			}
			for (; trailing < count; trailing += 1)
			{
				m_output.push_back('\n');
			}
			m_at_line_start = true;
		}

		// A blank line the author left between two lines is kept, collapsed to one,
		// except just inside an opening brace.
		void KeepAuthorBlankLine(const std::vector<Token>& tokens, size_t index)
		{
			if (index == 0u || !m_at_line_start || m_output.empty() || m_output.ends_with("{\n"))
			{
				return;
			}

			if (tokens[index].m_line - EndSourceLine(tokens[index - 1u]) >= 2)
			{
				EnsureTrailingNewlines(2);
			}
		}

		void WriteCommentLexeme(std::string_view lexeme)
		{
			for (size_t index = 0u; index < lexeme.size(); index += 1u)
			{
				const char ch = lexeme[index];
				m_output.push_back(ch);
				if (ch == '\n')
				{
					m_at_line_start = true;
					if (index + 1u < lexeme.size())
					{
						WriteCurrentIndent();
					}
				}
				else
				{
					m_at_line_start = false;
				}
			}
		}

		void FormatCommentToken(const std::vector<Token>& tokens, size_t index)
		{
			const Token& token = tokens[index];
			const Token* previous = PreviousNonCommentToken(tokens, index);
			const Token* next = NextNonCommentToken(tokens, index);
			const bool previous_same_line = previous != nullptr && EndSourceLine(*previous) == token.m_line;
			m_previous_was_unary = false;
			m_previous_was_type_bracket = false;
			if (!previous_same_line)
			{
				KeepAuthorBlankLine(tokens, index);
				// The first comment above a top-level declaration takes the blank line
				// that separates the declaration from what comes before it.
				const bool opens_declaration_comment = next != nullptr
					&& m_block_depth == 0 && m_paren_depth == 0 && m_bracket_depth == 0
					&& IsTopLevelStart(next->m_token_name)
					&& ClassifyTopLevel(next->m_token_name) == TopLevelCategory::Declaration
					&& !(index > 0u && IsComment(tokens[index - 1u].m_token_name));
				if (opens_declaration_comment && m_previous_token.has_value())
				{
					EnsureTrailingNewlines(2);
				}
			}
			const bool next_same_line = next != nullptr
				&& token.m_token_name == TokenName::BLOCK_COMMENT
				&& CountEmbeddedNewlines(token.m_lexeme) == 0
				&& next->m_line == token.m_line;

			if (previous_same_line)
			{
				const bool inline_block_comment =
					token.m_token_name == TokenName::BLOCK_COMMENT
					&& next != nullptr
					&& next->m_line == EndSourceLine(token);

				if (!m_output.empty() && m_output.back() != ' ' && m_output.back() != '\n')
				{
					m_output.append(inline_block_comment ? " " : "  ");
				}

				WriteCommentLexeme(token.m_lexeme);
				m_previous_token.reset();

				if (token.m_token_name == TokenName::LINE_COMMENT
					|| CountEmbeddedNewlines(token.m_lexeme) > 0
					|| next == nullptr
					|| next->m_line > EndSourceLine(token))
				{
					WriteNewline();
				}
				else
				{
					m_output.push_back(' ');
					m_at_line_start = false;
				}
				return;
			}

			if (next_same_line)
			{
				if (previous != nullptr && token.m_line > EndSourceLine(*previous) && !m_output.empty() && !m_at_line_start)
				{
					WriteNewline();
				}

				WriteCurrentIndent();
				WriteCommentLexeme(token.m_lexeme);
				m_output.push_back(' ');
				m_at_line_start = false;
				m_previous_token.reset();
				return;
			}

			if (!m_output.empty() && !m_at_line_start)
			{
				WriteNewline();
			}

			WriteCurrentIndent();
			WriteCommentLexeme(token.m_lexeme);
			m_previous_token.reset();

			if (token.m_token_name == TokenName::LINE_COMMENT || CountEmbeddedNewlines(token.m_lexeme) > 0)
			{
				WriteNewline();
			}
			else if (next != nullptr && next->m_line == token.m_line)
			{
				m_output.push_back(' ');
				m_at_line_start = false;
			}
			else
			{
				WriteNewline();
			}
		}

		// One blank line between top-level declarations, none inside the header of
		// module, import, use and export directives. A comment directly above a
		// declaration belongs to it, so the blank line goes above the comment.
		void EnsureSeparatedTopLevel(const std::vector<Token>& tokens, size_t index)
		{
			const TokenName token_name = tokens[index].m_token_name;
			if (m_block_depth != 0 || m_paren_depth != 0 || m_bracket_depth != 0 || !IsTopLevelStart(token_name))
			{
				return;
			}

			const TopLevelCategory category = ClassifyTopLevel(token_name);
			const bool in_header = category != TopLevelCategory::Declaration && m_last_top_level_category != TopLevelCategory::Declaration;
			const bool after_own_comment = index > 0u && IsComment(tokens[index - 1u].m_token_name) && m_at_line_start;
			if (m_previous_token.has_value() && !after_own_comment)
			{
				if (in_header)
				{
					if (!m_at_line_start)
					{
						WriteNewline();
					}
				}
				else
				{
					EnsureTrailingNewlines(2);
				}
			}

			m_last_top_level_category = category;
		}

		[[nodiscard]] bool IsAtTopLevelOfBlock() const
		{
			if (m_contexts.empty())
			{
				return false;
			}

			const Context& context = m_contexts.back();
			return context.m_kind == ContextKind::BlockBrace
				&& context.m_paren_depth == m_paren_depth
				&& context.m_bracket_depth == m_bracket_depth
				&& context.m_block_depth == m_block_depth;
		}

		void MaybeWriteSpace(TokenName current, bool is_type_bracket)
		{
			if (m_at_line_start)
			{
				return;
			}

			if (m_output.empty())
			{
				return;
			}

			if (m_output.back() == ' ' || m_output.back() == '\n')
			{
				return;
			}

			if (!m_previous_token.has_value())
			{
				return;
			}

			const TokenName previous = *m_previous_token;
			if (is_type_bracket || (m_previous_was_type_bracket && previous == TokenName::LEFT_ANGLE))
			{
				return;
			}

			if (current == TokenName::COMMA
				|| current == TokenName::SINGLE_SEMICOLON
				|| current == TokenName::RIGHT_PAREN
				|| current == TokenName::RIGHT_BRACKET
				|| current == TokenName::SINGLE_DOT
				|| current == TokenName::DOUBLE_DOT
				|| current == TokenName::DOUBLE_COLON
				|| current == TokenName::SINGLE_COLON)
			{
				return;
			}

			if (previous == TokenName::LEFT_PAREN
				|| previous == TokenName::LEFT_BRACKET
				|| previous == TokenName::SINGLE_DOT
				|| previous == TokenName::DOUBLE_DOT
				|| previous == TokenName::DOUBLE_COLON
				|| previous == TokenName::HASH)
			{
				return;
			}

			// A call or an index hugs what it applies to; after an infix operator the
			// parenthesis opens an operand and is spaced like one.
			// After `def`, `if`, `case` and `match` a parenthesis opens an operand, not a call.
			const bool opens_operand = (IsOperator(previous) && !m_previous_was_unary && !m_previous_was_type_bracket)
				|| previous == TokenName::DEF
				|| previous == TokenName::IF
				|| previous == TokenName::CASE
				|| previous == TokenName::MATCH;
			if ((current == TokenName::LEFT_PAREN || current == TokenName::LEFT_BRACKET) && !opens_operand)
			{
				return;
			}

			if (current == TokenName::RIGHT_BRACE)
			{
				return;
			}

			if (previous == TokenName::LEFT_BRACE && !m_contexts.empty() && m_contexts.back().m_kind == ContextKind::InlineBrace)
			{
				return;
			}

			// A unary operator hugs its operand: `-x`, `!done`, `-(a - b)`.
			if (m_previous_was_unary)
			{
				return;
			}

			m_output.push_back(' ');
		}

		[[nodiscard]] bool IsUnaryHere(TokenName current) const
		{
			if (!IsUnaryPrefix(current))
			{
				return false;
			}
			if (!m_last_code_token.has_value())
			{
				return true;
			}
			const TokenName previous = *m_last_code_token;
			return (IsOperator(previous) && !m_previous_was_type_bracket)
				|| previous == TokenName::LEFT_PAREN
				|| previous == TokenName::LEFT_BRACKET
				|| previous == TokenName::LEFT_BRACE
				|| previous == TokenName::COMMA
				|| previous == TokenName::SINGLE_SEMICOLON
				|| previous == TokenName::CASE
				|| previous == TokenName::HASH;
		}

		void WriteTokenText(const Token& token, bool is_type_bracket = false)
		{
			const bool is_unary = IsUnaryHere(token.m_token_name);
			WriteCurrentIndent();
			MaybeWriteSpace(token.m_token_name, is_type_bracket);
			m_output += TokenText(token);
			m_at_line_start = false;
			m_previous_token = token.m_token_name;
			m_last_code_token = token.m_token_name;
			m_previous_was_unary = is_unary;
			m_previous_was_type_bracket = is_type_bracket;
		}

		void FormatToken(const std::vector<Token>& tokens, size_t index)
		{
			const Token& token = tokens[index];
			const TokenName token_name = token.m_token_name;
			const Token* next_raw_token = index + 1u < tokens.size() ? &tokens[index + 1u] : nullptr;
			const Token* next_non_comment = NextNonCommentToken(tokens, index);
			const TokenName next_token = next_non_comment != nullptr ? next_non_comment->m_token_name : TokenName::END_OF_FILE;

			if (IsComment(token_name))
			{
				FormatCommentToken(tokens, index);
				return;
			}

			KeepAuthorBlankLine(tokens, index);
			EnsureSeparatedTopLevel(tokens, index);
			const bool is_type_bracket = m_type_brackets.contains(index);

			switch (token_name)
			{
			case TokenName::LEFT_BRACE:
			{
				const bool inline_brace = IsInlineBraceOpen(tokens, index);
				WriteCurrentIndent();
				MaybeWriteSpace(token_name, false);
				m_output.push_back('{');
				m_at_line_start = false;
				m_previous_token = token_name;
				m_last_code_token = token_name;
				m_previous_was_unary = false;
				m_previous_was_type_bracket = false;
				if (inline_brace)
				{
					m_contexts.push_back(Context{ ContextKind::InlineBrace, m_paren_depth, m_bracket_depth, m_block_depth });
					if (next_token != TokenName::RIGHT_BRACE)
					{
						m_output.push_back(' ');
					}
				}
				else
				{
					// A block that is a match arm's body indents from its `case`, not from
					// the line that opened the match.
					const bool is_arm_body = !m_match_contexts.empty()
						&& m_match_contexts.back().m_paren_depth == m_paren_depth
						&& m_match_contexts.back().m_bracket_depth == m_bracket_depth
						&& m_match_contexts.back().m_block_depth == m_block_depth;
					const int outer_indent = is_arm_body ? m_match_contexts.back().m_indent : m_indent;
					m_block_depth += 1;
					m_contexts.push_back(Context{ ContextKind::BlockBrace, m_paren_depth, m_bracket_depth, m_block_depth, outer_indent, m_indent });
					m_indent = outer_indent + 1;
					WriteNewline();
				}
				return;
			}
			case TokenName::RIGHT_BRACE:
			{
				if (!m_contexts.empty() && m_contexts.back().m_kind == ContextKind::InlineBrace)
				{
					if (!m_output.empty() && m_output.back() == ' ')
					{
						m_output.pop_back();
					}
					m_output += " }";
					m_at_line_start = false;
					// An import or export block ends its directive's line.
					if (next_raw_token == nullptr || !IsComment(next_raw_token->m_token_name) || next_raw_token->m_line != token.m_line)
					{
						WriteNewline();
					}
					m_previous_token = token_name;
					m_last_code_token = token_name;
					m_previous_was_unary = false;
					m_previous_was_type_bracket = false;
					m_contexts.pop_back();
					return;
				}

				if (!m_at_line_start)
				{
					WriteNewline();
				}
				const int restore_indent = m_contexts.empty() ? std::max(m_indent - 1, 0) : m_contexts.back().m_restore_indent;
				m_indent = m_contexts.empty() ? restore_indent : m_contexts.back().m_outer_indent;
				m_block_depth = std::max(m_block_depth - 1, 0);
				WriteCurrentIndent();
				m_indent = restore_indent;
				m_output.push_back('}');
				m_at_line_start = false;
				m_previous_token = token_name;
				m_last_code_token = token_name;
				m_previous_was_unary = false;
				m_previous_was_type_bracket = false;
				if (!m_contexts.empty())
				{
					m_contexts.pop_back();
				}
				if ((next_raw_token == nullptr || !IsComment(next_raw_token->m_token_name) || next_raw_token->m_line != token.m_line)
					&& next_token != TokenName::SINGLE_SEMICOLON
					&& next_token != TokenName::COMMA
					&& next_token != TokenName::ELSE)
				{
					WriteNewline();
				}
				return;
			}
			case TokenName::LEFT_PAREN:
				WriteTokenText(token);
				m_paren_depth += 1;
				return;
			case TokenName::RIGHT_PAREN:
				m_paren_depth = std::max(m_paren_depth - 1, 0);
				WriteTokenText(token);
				return;
			case TokenName::LEFT_BRACKET:
				WriteTokenText(token);
				m_bracket_depth += 1;
				return;
			case TokenName::RIGHT_BRACKET:
				m_bracket_depth = std::max(m_bracket_depth - 1, 0);
				WriteTokenText(token);
				return;
			case TokenName::COMMA:
				WriteTokenText(token);
				if (IsAtTopLevelOfBlock() && m_type_bracket_depth == 0)
				{
					WriteNewline();
				}
				else
				{
					m_output.push_back(' ');
				}
				return;
			case TokenName::SINGLE_SEMICOLON:
				WriteTokenText(token);
				while (!m_match_contexts.empty())
				{
					const MatchContext& context = m_match_contexts.back();
					if (context.m_paren_depth != m_paren_depth
						|| context.m_bracket_depth != m_bracket_depth
						|| context.m_block_depth != m_block_depth)
					{
						break;
					}

					m_match_contexts.pop_back();
				}
				if (next_raw_token == nullptr || !IsComment(next_raw_token->m_token_name) || next_raw_token->m_line != token.m_line)
				{
					WriteNewline();
				}
				return;
			case TokenName::CASE:
				if (!m_at_line_start)
				{
					WriteNewline();
				}
				if (!m_match_contexts.empty())
				{
					WriteIndent(m_match_contexts.back().m_indent);
				}
				else
				{
					WriteCurrentIndent();
				}
				m_output += TokenText(token);
				m_output.push_back(' ');
				m_at_line_start = false;
				m_previous_token = token_name;
				m_last_code_token = token_name;
				m_previous_was_unary = false;
				m_previous_was_type_bracket = false;
				return;
			case TokenName::WITH:
				WriteTokenText(token);
				if (next_token == TokenName::CASE)
				{
					m_match_contexts.push_back(MatchContext{ m_indent + 1, m_paren_depth, m_bracket_depth, m_block_depth });
					WriteNewline();
				}
				return;
			case TokenName::MODULE:
			case TokenName::IMPORT:
			case TokenName::USE:
			case TokenName::PUBLIC:
			case TokenName::PRIVATE:
			case TokenName::DEF:
			case TokenName::CLASS:
			case TokenName::INSTANCE:
			case TokenName::TYPE:
			case TokenName::ALIAS:
			case TokenName::FOREIGN:
				WriteTokenText(token);
				m_last_top_level_category = ClassifyTopLevel(token_name);
				return;
			default:
				WriteTokenText(token, is_type_bracket);
				if (is_type_bracket)
				{
					m_type_bracket_depth += token_name == TokenName::LEFT_ANGLE ? 1 : (token_name == TokenName::RIGHT_SHIFT ? -2 : -1);
				}
				if ((token_name == TokenName::THIN_ARROW || token_name == TokenName::FAT_ARROW) && next_token == TokenName::LEFT_BRACE)
				{
					m_output.push_back(' ');
				}
				return;
			}
		}
	};

	[[nodiscard]] std::vector<Token> TokensWithoutEof(TokenStream&& stream)
	{
		std::vector<Token> tokens;
		tokens.reserve(static_cast<size_t>(stream.Size()));
		for (int index = 0; index < stream.Size(); index += 1)
		{
			Token& token = stream[index];
			if (token.m_token_name == TokenName::END_OF_FILE)
			{
				continue;
			}

			tokens.emplace_back(token.m_lexeme, token.m_token_name, token.m_line, token.m_file_name, token.m_column.value_or(0), token.m_source_length.value_or(0u));
		}
		return tokens;
	}

	[[nodiscard]] std::expected<std::string, std::string> ReadFileText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			return std::unexpected(std::format("Could not open file: {}", path.string()));
		}

		std::ostringstream buffer;
		buffer << input.rdbuf();
		if (!buffer)
		{
			return std::unexpected(std::format("Could not read file: {}", path.string()));
		}

		return buffer.str();
	}

	[[nodiscard]] std::vector<std::filesystem::path> CollectMdrFiles(const std::filesystem::path& target_path)
	{
		std::vector<std::filesystem::path> files;
		std::error_code error_code;
		if (std::filesystem::is_regular_file(target_path, error_code))
		{
			files.push_back(target_path);
			return files;
		}

		if (!std::filesystem::is_directory(target_path, error_code))
		{
			return files;
		}

		for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(target_path))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}

			if (entry.path().extension() == ".mmt")
			{
				files.push_back(entry.path());
			}
		}

		std::ranges::sort(files);
		return files;
	}
}

namespace MidoriFormatter
{
	bool RunResult::HasErrors() const
	{
		return std::ranges::any_of(m_files, [](const FileResult& result) { return result.m_error.has_value(); });
	}

	bool RunResult::HasChanges() const
	{
		return std::ranges::any_of(m_files, [](const FileResult& result) { return result.m_changed; });
	}

	int RunResult::ErrorCount() const
	{
		return static_cast<int>(std::ranges::count_if(m_files, [](const FileResult& result) { return result.m_error.has_value(); }));
	}

	int RunResult::ChangedCount() const
	{
		return static_cast<int>(std::ranges::count_if(m_files, [](const FileResult& result) { return result.m_changed; }));
	}

	std::expected<std::string, CompilerError> FormatSource(std::string_view source_code, std::string_view file_name)
	{
		MidoriResult::LexerResult lex_result = Lexer(
			std::string(source_code),
			file_name,
			Lexer::Options{ .m_preserve_comments = true }).Lex();
		if (!lex_result.has_value())
		{
			return std::unexpected(std::move(lex_result.error()));
		}

		FormatterEngine engine;
		return engine.Format(TokensWithoutEof(std::move(lex_result.value())));
	}

	RunResult FormatPath(const std::filesystem::path& target_path, const Options& options)
	{
		RunResult result;
		const std::vector<std::filesystem::path> files = CollectMdrFiles(target_path);
		for (const std::filesystem::path& path : files)
		{
			FileResult file_result;
			file_result.m_path = path;

			const std::expected<std::string, std::string> read_result = ReadFileText(path);
			if (!read_result.has_value())
			{
				file_result.m_error = read_result.error();
				result.m_files.push_back(std::move(file_result));
				continue;
			}

			file_result.m_original_text = read_result.value();
			const std::expected<std::string, CompilerError> format_result = FormatSource(file_result.m_original_text, path.string());
			if (!format_result.has_value())
			{
				file_result.m_error = std::string(format_result.error().Rendered());
				result.m_files.push_back(std::move(file_result));
				continue;
			}

			// A file keeps the byte order mark it came with: formatting a file is
			// not a decision about its encoding.
			file_result.m_formatted_text = MidoriSource::StartsWithByteOrderMark(file_result.m_original_text)
				? std::string(MidoriSource::UTF8_BYTE_ORDER_MARK) + format_result.value()
				: format_result.value();
			file_result.m_changed = file_result.m_formatted_text != file_result.m_original_text;
			if (options.m_write_in_place && file_result.m_changed)
			{
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				if (!output.is_open())
				{
					file_result.m_error = std::format("Could not open file for writing: {}", path.string());
				}
				else
				{
					output.write(file_result.m_formatted_text.data(), static_cast<std::streamsize>(file_result.m_formatted_text.size()));
					if (!output)
					{
						file_result.m_error = std::format("Could not write formatted file: {}", path.string());
					}
					else
					{
						file_result.m_written = true;
					}
				}
			}

			result.m_files.push_back(std::move(file_result));
		}

		return result;
	}
}
