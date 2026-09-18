// Sherlock — tools/Sherlock/Source/DocumentIndex/include/DocumentIndex/Heading.h
// One markdown heading: its line, level, optional section number and title, fence-aware (derived).
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Sherlock::DocumentIndex
{
    struct Heading
    {
        std::size_t                Line  = 0; // 1-based, the '#' line itself
        int                         Level = 0; // count of leading '#'
        std::optional<std::string> Number;    // "5", "5.1", "10" -- '§' and one trailing '.' stripped
        std::string                Title;      // the text after the number, or the whole heading text
    };

    // A line inside a fenced code block (``` or ~~~, closed by its own opening marker) is never a
    // heading, however many '#' it starts with -- illustrating a heading inside a code sample must
    // not split the document, and the measured corpus has 132 such lines (docs/re + docs/concepts).
    std::vector<Heading> ParseHeadings(std::string_view markdown);
}
