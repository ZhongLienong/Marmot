#include "support/SourceFixture.h"

#include <sstream>
#include <utility>

namespace MidoriTest
{
	SourceFixture::SourceFixture(std::string source_code, std::string file_name)
		: m_source_code(std::move(source_code)),
		m_file_name(std::move(file_name)),
		m_source_lines(SplitSourceLines(m_source_code))
	{
	}

	const std::string& SourceFixture::SourceCode() const
	{
		return m_source_code;
	}

	const std::string& SourceFixture::FileName() const
	{
		return m_file_name;
	}

	const std::vector<std::string>& SourceFixture::SourceLines() const
	{
		return m_source_lines;
	}

	std::vector<std::string> SourceFixture::SplitSourceLines(const std::string& source_code)
	{
		std::vector<std::string> source_lines;
		std::istringstream stream(source_code);
		std::string line;
		while (std::getline(stream, line))
		{
			source_lines.emplace_back(line);
		}

		return source_lines;
	}
}
