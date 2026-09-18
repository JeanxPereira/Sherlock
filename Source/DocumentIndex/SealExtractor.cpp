// Sherlock — tools/Sherlock/Source/DocumentIndex/SealExtractor.cpp
// blocks()/TAG/BIN/ADDR, ported from lint_seals.py for Source/'s "//"-only comment style.
#include <DocumentIndex/SealExtractor.h>

#include <charconv>
#include <fstream>
#include <optional>
#include <regex>
#include <utility>

namespace Sherlock::DocumentIndex
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        struct Block
        {
            std::size_t FirstLine = 0; // 1-based
            std::string Text;
        };

        struct TagMatch
        {
            std::size_t Position = 0;
            std::size_t Length   = 0;
            std::string Tag;
        };

        Foundation::Expected<std::string> ReadWhole(std::istream&     stream,
                                                     std::string_view operation,
                                                     std::string_view subject)
        {
            std::string content;
            char        character = 0;
            try
            {
                while (stream.get(character))
                {
                    content.push_back(character);
                }
            }
            catch (const std::ios_base::failure&)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, std::string(operation), std::string(subject),
                            "the file cannot be read completely", "check it is readable");
            }
            if (!stream.eof() && stream.fail())
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, std::string(operation), std::string(subject),
                            "the file cannot be read completely", "check it is readable");
            }
            return content;
        }

        // Splits on '\n' and drops a trailing '\r', the same normalisation Python's universal
        // newline handling applies before lint_seals.py ever sees a line.
        std::vector<std::string> SplitLines(std::string_view text)
        {
            std::vector<std::string> lines;
            std::size_t               start = 0;
            while (start <= text.size())
            {
                const auto newline = text.find('\n', start);
                auto       line    = newline == std::string_view::npos ? text.substr(start)
                                                                        : text.substr(start, newline - start);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                lines.emplace_back(line);
                if (newline == std::string_view::npos)
                {
                    break;
                }
                start = newline + 1;
            }
            return lines;
        }

        // Leading spaces/tabs only -- lint_seals.py's COMMENT regex is anchored the same way and
        // Source/'s "//" is the only comment style this port reads (see the header comment).
        bool IsCommentLine(std::string_view line)
        {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            {
                ++i;
            }
            return line.substr(i).starts_with("//");
        }

        std::string_view TrimBoth(std::string_view line)
        {
            std::size_t a = 0, b = line.size();
            while (a < b && (line[a] == ' ' || line[a] == '\t'))
            {
                ++a;
            }
            while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t'))
            {
                --b;
            }
            return line.substr(a, b - a);
        }

        // Mirrors lint_seals.py's blocks(): a run of comment lines is one block. A code line whose
        // own "//" sits within the first 8 characters of its TRIMMED content -- not its raw,
        // still-indented one -- while a block is already open and the line is not brace-only,
        // extends that block (a short statement glued to the paragraph above it). The trim matters:
        // lint_seals.py computes the offset on `ln.strip()`, so an 8-space-indented file (this one)
        // would wrongly refuse every continuation if the offset were measured on the raw line
        // instead -- the gate's "GlueTest" fixture entry exists to catch exactly that regression.
        // Otherwise a lone trailing "//" on a code line is its own one-line block.
        std::vector<Block> SplitBlocks(const std::vector<std::string>& lines)
        {
            std::vector<Block>       blocks;
            std::vector<std::string> current;
            std::size_t               start = 0;
            const auto                flush = [&]
            {
                if (!current.empty())
                {
                    std::string text;
                    for (std::size_t i = 0; i < current.size(); ++i)
                    {
                        if (i > 0)
                        {
                            text += '\n';
                        }
                        text += current[i];
                    }
                    blocks.push_back({start, std::move(text)});
                    current.clear();
                }
            };
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                const auto& line        = lines[i];
                const auto  lineNumber  = i + 1;
                const auto  trimmed     = TrimBoth(line);
                const auto  trimmedSlash = trimmed.find("//");
                const bool  continues   = !current.empty() && !trimmed.empty() && !trimmed.starts_with("{") &&
                                          !trimmed.starts_with("}") && trimmedSlash != std::string_view::npos &&
                                          trimmedSlash < 8;
                if (IsCommentLine(line))
                {
                    if (current.empty())
                    {
                        start = lineNumber;
                    }
                    current.push_back(line);
                }
                else if (continues)
                {
                    current.push_back(line);
                }
                else
                {
                    flush();
                    const auto rawSlash = line.find("//");
                    if (rawSlash != std::string::npos)
                    {
                        blocks.push_back({lineNumber, line.substr(rawSlash)});
                    }
                }
            }
            flush();
            return blocks;
        }

        std::size_t LineOfOffset(const Block& block, std::size_t offset)
        {
            std::size_t line = block.FirstLine;
            for (std::size_t i = 0; i < offset && i < block.Text.size(); ++i)
            {
                if (block.Text[i] == '\n')
                {
                    ++line;
                }
            }
            return line;
        }

        // The image token right after the BIN tag -- lint_seals.py's own BIN regex is the only one
        // that ever captures this group, so this is only ever tried for a "BIN" tag match.
        // match_continuous refuses a match that does not start exactly at `afterTag`: an image
        // three words into the sentence is not this seal's image.
        std::optional<std::pair<std::string, std::size_t>> TryImage(const std::string& text, std::size_t afterTag)
        {
            static const std::regex imagePattern(R"(\s+([A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)*))");
            std::smatch              match;
            if (afterTag <= text.size() &&
                std::regex_search(text.cbegin() + static_cast<std::ptrdiff_t>(afterTag), text.cend(), match,
                                  imagePattern, std::regex_constants::match_continuous))
            {
                return std::make_pair(match[1].str(), static_cast<std::size_t>(match.length(0)));
            }
            return std::nullopt;
        }
    }

    Foundation::Expected<std::vector<SealRow>> ExtractSeals(const std::filesystem::path& file,
                                                             std::string_view             repoRelativePath)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "ExtractSeals", file.string(),
                        "the file cannot be opened", "check it exists and is readable");
        }
        return ExtractSeals(stream, repoRelativePath, file.string());
    }

    Foundation::Expected<std::vector<SealRow>> ExtractSeals(std::istream&     stream,
                                                            std::string_view repoRelativePath,
                                                            std::string_view diagnosticSubject)
    {
        auto content = ReadWhole(stream, "ExtractSeals", diagnosticSubject);
        if (!content)
        {
            return std::unexpected(content.error());
        }
        const auto lines = SplitLines(*content);

        static const std::regex tagPattern(R"(\[(BIN|KIT|OBS|API)\])");
        static const std::regex cacheAddress(R"(0x(1[89a-f][0-9a-f]{7}|2[0-9a-f]{8})\b)");

        std::vector<SealRow> seals;
        for (const auto& block : SplitBlocks(lines))
        {
            std::vector<TagMatch> tags;
            for (auto it = std::sregex_iterator(block.Text.begin(), block.Text.end(), tagPattern);
                 it != std::sregex_iterator(); ++it)
            {
                tags.push_back({static_cast<std::size_t>(it->position(0)), static_cast<std::size_t>(it->length(0)),
                                (*it)[1].str()});
            }
            for (std::size_t i = 0; i < tags.size(); ++i)
            {
                const auto& tag      = tags[i];
                const auto  afterTag = tag.Position + tag.Length;

                std::optional<std::string> image;
                auto                        afterImage = afterTag;
                if (tag.Tag == "BIN")
                {
                    if (const auto found = TryImage(block.Text, afterTag))
                    {
                        image       = found->first;
                        afterImage += found->second;
                    }
                }

                // The next occurrence of the SAME tag closes this one's segment -- lint_seals.py's
                // own `bin_matches[idx + 1]` chaining for the BIN tag, generalised here to every
                // tag kind (a Sherlock-only widening for KIT/OBS/API; see the header comment).
                std::size_t segEnd = block.Text.size();
                for (std::size_t j = i + 1; j < tags.size(); ++j)
                {
                    if (tags[j].Tag == tag.Tag)
                    {
                        segEnd = tags[j].Position;
                        break;
                    }
                }

                SealRow row;
                row.File = std::string(repoRelativePath);
                row.Line = LineOfOffset(block, tag.Position);
                row.Tag  = tag.Tag;
                row.Image = std::move(image);

                if (afterImage < segEnd)
                {
                    std::cmatch addressMatch;
                    const char* segmentBegin = block.Text.data() + afterImage;
                    const char* segmentEnd   = block.Text.data() + segEnd;
                    if (std::regex_search(segmentBegin, segmentEnd, addressMatch, cacheAddress))
                    {
                        std::uint64_t value = 0;
                        const auto     text  = addressMatch.str();
                        std::from_chars(text.data() + 2, text.data() + text.size(), value, 16);
                        row.Address = value;
                    }
                }
                seals.push_back(std::move(row));
            }
        }
        return seals;
    }
}
