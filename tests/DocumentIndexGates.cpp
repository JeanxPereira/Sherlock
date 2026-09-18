// Sherlock — tests/Sherlock/DocumentIndexGates.cpp
// Unit gates for DocumentIndex's pure parsers, over the fixtures in tests/Sherlock/fixtures/.
#include "SherlockHarness.h"

#include <DocumentIndex/Heading.h>

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace Sherlock;

namespace
{
    std::string ReadFixture(const char* name)
    {
        std::ifstream stream(std::filesystem::path(__FILE__).parent_path() / "fixtures" / name);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }

    void TestHeadingParsing()
    {
        const auto headings = DocumentIndex::ParseHeadings(ReadFixture("heading-sample.md"));
        ExpectEq(headings.size(), std::size_t(10),
                "heading count (2 inside the first fence, 1 four-space-indented, 1 tab-indented and 1 "
                "inside a longer fence are excluded; the real heading after that fence still counts)");
        ExpectEq(headings[0].Number.value_or(""), "5", "'# §5.' numbers as 5");
        ExpectEq(headings[0].Title, "The inversion: the values", "'# §5.' title");
        ExpectEq(headings[1].Number.value_or(""), "5.1", "'## 5.1' numbers as 5.1");
        Expect(headings[3].Level == 3, "'### Two families...' is level 3");
        Expect(!headings[3].Number.has_value(), "'### Two families...' is unnumbered");
        Expect(!headings[8].Number.has_value(), "'# §9b.' does not match the strict numbered form");
        ExpectEq(headings[8].Title, "\xC2\xA7" "9b. The VERTICAL axis closed: `Alignment.center`",
                "'# §9b.' keeps its raw text as Title");

        const bool sawIndented =
            std::any_of(headings.begin(), headings.end(), [](const DocumentIndex::Heading& heading)
                        { return heading.Title.find("Indented heading") != std::string::npos; });
        Expect(!sawIndented, "a line indented 4 spaces is a CommonMark code line, never a heading");

        const bool sawTabIndented =
            std::any_of(headings.begin(), headings.end(), [](const DocumentIndex::Heading& heading)
                        { return heading.Title.find("Tab-indented heading") != std::string::npos; });
        Expect(!sawTabIndented, "a leading tab reaches column 4 and is also a CommonMark code line");

        const bool sawInnerFenceHeading =
            std::any_of(headings.begin(), headings.end(), [](const DocumentIndex::Heading& heading)
                        { return heading.Title.find("still inside the outer fence") != std::string::npos; });
        Expect(!sawInnerFenceHeading, "a 3-backtick line does not close a 4-backtick fence");

        ExpectEq(headings[9].Title, "Heading after the fence, definitely real",
                "the real heading after the correctly-closed outer fence is still found");
    }
}

int main()
{
    TestHeadingParsing();
    return Finish();
}
