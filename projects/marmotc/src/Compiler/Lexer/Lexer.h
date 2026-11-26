#pragma once

#include "Compiler/Result/Result.h"

#include <string>
#include <string_view>

class Lexer
{
public:
	struct Options
	{
		bool m_preserve_comments = false;
	};

	Lexer(std::string&& source_code, std::string_view file_name) noexcept;
	Lexer(std::string&& source_code, std::string_view file_name, Options options) noexcept;

	MarmotResult::LexerResult Lex() const;

private:
	std::string m_source_code;
	std::string m_file_name;
	Options m_options;
};
