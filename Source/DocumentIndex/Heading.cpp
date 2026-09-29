// Sherlock — Source/DocumentIndex/Heading.cpp
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

        // CommonMark: a leading tab expands to the next multiple of four columns. A line whose
        // leading whitespace reaches column 4 is an indented code line, never a heading, however
        // many '#' characters follow it.
        std::size_t LeadingIndentWidth(std::string_view line)
        {
            std::size_t column = 0;
            for (const char ch : line)
            {
                if (ch == ' ')
                {
                    ++column;
                }
                else if (ch == '\t')
                {
                    column = (column / 4 + 1) * 4;
                }
                else
                {
                    break;
                }
            }
            return column;
        }

        struct FenceMarker
        {
            char        Char   = '\0';
            std::size_t Length = 0;
        };

        // A fence opens on a run of three or more of the same character, backtick or tilde.
        std::optional<FenceMarker> ParseFenceOpen(std::string_view trimmed)
        {
            if (trimmed.empty() || (trimmed[0] != '`' && trimmed[0] != '~'))
            {
                return std::nullopt;
            }
            const char  marker = trimmed[0];
            std::size_t length = 0;
            while (length < trimmed.size() && trimmed[length] == marker)
            {
                ++length;
            }
            if (length < 3)
            {
                return std::nullopt;
            }
            return FenceMarker{marker, length};
        }

        // A fence closes on a line whose run is the SAME character as the opening one, at least
        // as long, with nothing but whitespace after it. A shorter run, or one of the other
        // fence character, is content -- it stays inside the fence.
        bool ClosesFence(std::string_view trimmed, const FenceMarker& opening)
        {
            if (trimmed.empty() || trimmed[0] != opening.Char)
            {
                return false;
            }
            std::size_t length = 0;
            while (length < trimmed.size() && trimmed[length] == opening.Char)
            {
                ++length;
            }
            if (length < opening.Length)
            {
                return false;
            }
            for (std::size_t i = length; i < trimmed.size(); ++i)
            {
                if (trimmed[i] != ' ' && trimmed[i] != '\t')
                {
                    return false;
                }
            }
            return true;
        }

        // '§' is UTF-8 0xC2 0xA7; std::regex's ECMAScript grammar accepts the \xHH hex escape.
        // Covers every numbered form measured in docs/re: "§5. Title" (a period, then a title),
        // "§10 Title" (no period, more text follows), "5.1 Title" (no '§'). The trailing `\s+`
        // REQUIRES at least one title character after the number -- a heading numbered but with
        // no title text at all (a bare "§5." with nothing following) does not match this pattern
        // either, and falls through to the unnumbered case below, keeping its raw text as Title.
        // A form outside the numbered shape (e.g. "§9b.") does the same -- graceful, not a crash;
        // the sample fixture carries exactly that case.
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
        bool                   inFence = false;
        FenceMarker            fence;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            const auto trimmed = TrimLeft(lines[i]);
            if (!inFence)
            {
                if (const auto opened = ParseFenceOpen(trimmed))
                {
                    inFence = true;
                    fence   = *opened;
                    continue;
                }
            }
            else
            {
                if (ClosesFence(trimmed, fence))
                {
                    inFence = false;
                }
                continue;
            }
            if (LeadingIndentWidth(lines[i]) >= 4)
            {
                continue; // an indented code line (CommonMark), never a heading
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
