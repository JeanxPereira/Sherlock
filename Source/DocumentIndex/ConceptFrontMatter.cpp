// Sherlock — Source/DocumentIndex/ConceptFrontMatter.cpp
// A hand-rolled parser for the three-key shape -- no YAML dependency for one bracket list.
#include <DocumentIndex/ConceptFrontMatter.h>

#include <cctype>

namespace Sherlock::DocumentIndex
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        std::string_view Trim(std::string_view text)
        {
            std::size_t begin = 0, end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
            {
                ++begin;
            }
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        std::vector<std::string> ParseAliasList(std::string_view value)
        {
            std::vector<std::string> aliases;
            value = Trim(value);
            if (value.size() >= 2 && value.front() == '[' && value.back() == ']')
            {
                value = value.substr(1, value.size() - 2);
            }
            std::size_t start = 0;
            while (start <= value.size())
            {
                const auto comma = value.find(',', start);
                const auto piece = Trim(value.substr(
                    start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
                if (!piece.empty())
                {
                    aliases.emplace_back(piece);
                }
                if (comma == std::string_view::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            return aliases;
        }
    }

    Foundation::Expected<FrontMatter> ParseFrontMatter(std::string_view text)
    {
        FrontMatter result;
        if (!text.starts_with("---\n") && !text.starts_with("---\r\n"))
        {
            return result;
        }
        const auto firstLineEnd = text.find('\n');
        const auto close        = text.find("\n---", firstLineEnd);
        if (close == std::string_view::npos)
        {
            return Fail(DiagnosticCode::Malformed, Severity::Failed, "ParseFrontMatter", "front matter",
                        "the closing '---' line is missing", "close the front matter block");
        }
        const auto body = text.substr(firstLineEnd + 1, close - firstLineEnd - 1);
        std::size_t lineStart = 0;
        while (lineStart <= body.size())
        {
            const auto lineEnd = body.find('\n', lineStart);
            const auto line    = body.substr(
                lineStart, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineStart);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos)
            {
                const auto key   = Trim(line.substr(0, colon));
                const auto value = Trim(line.substr(colon + 1));
                if (key == "type")
                {
                    result.Type = std::string(value);
                }
                else if (key == "title")
                {
                    result.Title = std::string(value);
                }
                else if (key == "aliases")
                {
                    result.Aliases = ParseAliasList(value);
                }
            }
            if (lineEnd == std::string_view::npos)
            {
                break;
            }
            lineStart = lineEnd + 1;
        }
        const auto afterClose = text.find('\n', close + 1);
        result.BodyStart       = afterClose == std::string_view::npos ? text.size() : afterClose + 1;
        return result;
    }

    std::size_t GeneratedBlockStart(std::string_view text)
    {
        const auto marker = text.find("<!-- GENERATED");
        return marker == std::string_view::npos ? text.size() : marker;
    }
}
