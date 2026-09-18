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

    // A line inside a fenced code block (``` or ~~~, closed only by a line whose run is the SAME
    // character and at least as long as the opening one) is never a heading, however many '#' it
    // starts with. Neither is a line indented 4 or more columns (CommonMark: a leading tab
    // expands to the next multiple of four) -- an indented code line, not an ATX heading.
    std::vector<Heading> ParseHeadings(std::string_view markdown);
}
