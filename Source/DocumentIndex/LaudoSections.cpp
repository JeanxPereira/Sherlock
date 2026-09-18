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
            if (!stream.eof() && stream.fail())
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "SplitDocument", file.string(),
                            "the file cannot be read completely", "check it is readable");
            }
            return buffer.str();
        }

        std::vector<std::size_t> LineOffsets(std::string_view text)
        {
            std::vector<std::size_t> offsets{0};
            for (std::size_t offset = 0; offset < text.size(); ++offset)
            {
                if (text[offset] == '\n' && offset + 1 < text.size())
                {
                    offsets.push_back(offset + 1);
                }
            }
            return offsets;
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
        const std::string_view text = *content;
        const auto lineOffsets = LineOffsets(text);

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
            section.LastLine  = lineOffsets.size();
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
            std::size_t last = lineOffsets.size();
            std::size_t endOffset = text.size();
            for (std::size_t j = i + 1; j < headings.size(); ++j)
            {
                if (headings[j].Level <= headings[i].Level)
                {
                    last = headings[j].Line - 1;
                    endOffset = lineOffsets[headings[j].Line - 1];
                    break;
                }
            }
            DocSection section;
            section.File      = std::string(repoRelativePath);
            section.Number     = headings[i].Number;
            section.Title      = headings[i].Title;
            section.FirstLine  = headings[i].Line;
            section.LastLine   = last;
            section.Text       = std::string(text.substr(lineOffsets[section.FirstLine - 1], endOffset - lineOffsets[section.FirstLine - 1]));
            sections.push_back(std::move(section));
        }
        return sections;
    }
}
