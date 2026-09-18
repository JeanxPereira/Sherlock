// Sherlock — tools/Sherlock/Source/DocumentIndex/LaudoSections.cpp
// Ties Heading and ConceptFrontMatter together into one file's DocSection rows.
#include <DocumentIndex/LaudoSections.h>

#include <DocumentIndex/ConceptFrontMatter.h>
#include <DocumentIndex/Heading.h>

#include <fstream>
#include <sstream>

namespace Sherlock::DocumentIndex
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        Foundation::Expected<std::string> ReadWhole(const std::filesystem::path& file)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "SplitDocument", file.string(),
                            "the file cannot be opened", "check it exists and is readable");
            }
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            return buffer.str();
        }

        std::vector<std::string_view> SliceLines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            std::size_t                    start = 0;
            while (start <= text.size())
            {
                const auto newline = text.find('\n', start);
                lines.push_back(newline == std::string_view::npos ? text.substr(start)
                                                                    : text.substr(start, newline - start));
                if (newline == std::string_view::npos)
                {
                    break;
                }
                start = newline + 1;
            }
            return lines;
        }

        std::string JoinLines(const std::vector<std::string_view>& lines, std::size_t firstLine, std::size_t lastLine)
        {
            std::string text;
            for (std::size_t n = firstLine; n <= lastLine && n <= lines.size(); ++n)
            {
                if (n > firstLine)
                {
                    text += '\n';
                }
                text += lines[n - 1];
            }
            return text;
        }
    }

    Foundation::Expected<std::vector<DocSection>> SplitDocument(const std::filesystem::path& file,
                                                                std::string_view              repoRelativePath)
    {
        auto content = ReadWhole(file);
        if (!content)
        {
            return std::unexpected(content.error());
        }
        const std::string_view text  = *content;
        const auto             lines = SliceLines(text);

        if (repoRelativePath.find("docs/concepts/") != std::string_view::npos)
        {
            auto frontMatter = ParseFrontMatter(text);
            if (!frontMatter)
            {
                return std::unexpected(frontMatter.error());
            }
            DocSection section;
            section.File      = std::string(repoRelativePath);
            section.Title     = frontMatter->Title.empty() ? std::string(repoRelativePath) : frontMatter->Title;
            section.FirstLine = 1;
            section.LastLine  = lines.size();
            section.Text      = std::string(text);
            return std::vector<DocSection>{std::move(section)};
        }

        const auto headings = ParseHeadings(text);
        if (headings.empty())
        {
            return std::vector<DocSection>{};
        }
        std::vector<DocSection> sections;
        sections.reserve(headings.size());
        for (std::size_t i = 0; i < headings.size(); ++i)
        {
            std::size_t last = lines.size();
            for (std::size_t j = i + 1; j < headings.size(); ++j)
            {
                if (headings[j].Level <= headings[i].Level)
                {
                    last = headings[j].Line - 1;
                    break;
                }
            }
            DocSection section;
            section.File      = std::string(repoRelativePath);
            section.Number     = headings[i].Number;
            section.Title      = headings[i].Title;
            section.FirstLine  = headings[i].Line;
            section.LastLine   = last;
            section.Text       = JoinLines(lines, section.FirstLine, section.LastLine);
            sections.push_back(std::move(section));
        }
        return sections;
    }
}
