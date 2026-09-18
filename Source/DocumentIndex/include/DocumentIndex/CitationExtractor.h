// Sherlock — tools/Sherlock/Source/DocumentIndex/include/DocumentIndex/CitationExtractor.h
// Address and symbol citations found in one section's text (derived).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Sherlock::DocumentIndex
{
    struct Citation
    {
        std::optional<std::uint64_t> Address;
        std::optional<std::string>   Symbol;
    };

    // Every distinct 0x-address in `text` (backticked or bare) becomes an Address citation.
    // Every distinct backtick-quoted token shaped like a mangled Swift name (`_$s...`) or a
    // dotted Apple-style chain (`Word.Word...`) becomes a Symbol citation, EXCLUDING a token
    // ending in a known file extension (a header or script named in prose, not a symbol -- of
    // 2 713 such tokens measured across docs/re + docs/concepts, 1 304 are file names) or one
    // containing a path separator. Deduplicated; a Citation never carries both fields.
    std::vector<Citation> ExtractCitations(std::string_view text);
}
