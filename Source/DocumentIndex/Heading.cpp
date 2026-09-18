// Sherlock — tools/Sherlock/Source/DocumentIndex/Heading.cpp
// Splits markdown into lines, walks fences, classifies each heading line into Number/Title.
#include <DocumentIndex/Heading.h>

#include <cctype>
#include <regex>

namespace Sherlock::DocumentIndex
{
    namespace
    {
        std::vector<std::string_view> SplitLines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            std::size_t                   start = 0;
            while (start <= text.size())
            {
                const auto newline = text.find('\n', start);
                auto       line    = newline == std::string_view::npos ? text.substr(start)
                                                                        : text.substr(start, newline - start);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                lines.push_back(line);
                if (newline == std::string_view::npos)
                {
                    break;
                }
                start = newline + 1;
            }
            return lines;
        }

        std::string_view TrimLeft(std::string_view text)
        {
            std::size_t i = 0;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
            {
                ++i;
            }
            return text.substr(i);
        }

        // '§' is UTF-8 0xC2 0xA7; std::regex's ECMAScript grammar accepts the \xHH hex escape.
        // Covers every numbered form measured in docs/re: "§5." (bare), "§10 Title" (no period,
        // more text follows), "5.1 Title" (no '§'). A form outside this (e.g. "§9b.") does not
        // match and falls through to the unnumbered case below, keeping its raw text as Title --
        // graceful, not a crash; the sample fixture carries exactly this case.
        const std::regex& NumberedPattern()
        {
            static const std::regex pattern(R"(^(?:\xC2\xA7)?(\d+(?:\.\d+)*)\.?\s+(.*)$)");
            return pattern;
        }
    }

    std::vector<Heading> ParseHeadings(std::string_view markdown)
    {
        std::vector<Heading> headings;
        const auto            lines = SplitLines(markdown);
        bool                   inFence     = false;
        std::string_view       fenceMarker;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            const auto trimmed = TrimLeft(lines[i]);
            if (!inFence && (trimmed.starts_with("```") || trimmed.starts_with("~~~")))
            {
                inFence     = true;
                fenceMarker = trimmed.substr(0, 3);
                continue;
            }
            if (inFence)
            {
                if (trimmed.starts_with(fenceMarker))
                {
                    inFence = false;
                }
                continue;
            }
            std::size_t level = 0;
            while (level < trimmed.size() && trimmed[level] == '#')
            {
                ++level;
            }
            if (level == 0 || level > 6 || level >= trimmed.size() || trimmed[level] != ' ')
            {
                continue;
            }
            const std::string rest(TrimLeft(trimmed.substr(level + 1)));
            Heading            heading;
            heading.Line  = i + 1;
            heading.Level = static_cast<int>(level);
            std::smatch match;
            if (std::regex_match(rest, match, NumberedPattern()))
            {
                heading.Number = match[1].str();
                heading.Title  = match[2].str();
            }
            else
            {
                heading.Title = rest;
            }
            headings.push_back(std::move(heading));
        }
        return headings;
    }
}
