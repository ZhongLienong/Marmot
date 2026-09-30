#pragma once

#include <string>
#include <vector>

namespace MidoriTest
{
	class SourceFixture
	{
	private:
		std::string m_source_code;
		std::string m_file_name;
		std::vector<std::string> m_source_lines;

	public:
		explicit SourceFixture(std::string source_code, std::string file_name = "Test.mmt");

		[[nodiscard]] const std::string& SourceCode() const;

		[[nodiscard]] const std::string& FileName() const;

		[[nodiscard]] const std::vector<std::string>& SourceLines() const;

	private:
		static std::vector<std::string> SplitSourceLines(const std::string& source_code);
	};
}
