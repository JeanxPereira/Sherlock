// Sherlock — tests/Sherlock/DocumentIndexGates.cpp
// Unit gates for DocumentIndex's pure parsers, over the fixtures in tests/Sherlock/fixtures/.
#include "SherlockHarness.h"

#include <DocumentIndex/Heading.h>

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
        ExpectEq(headings.size(), std::size_t(9), "heading count (2 inside the fence are excluded)");
        ExpectEq(headings[0].Number.value_or(""), "5", "'# §5.' numbers as 5");
        ExpectEq(headings[0].Title, "The inversion: the values", "'# §5.' title");
        ExpectEq(headings[1].Number.value_or(""), "5.1", "'## 5.1' numbers as 5.1");
        Expect(headings[3].Level == 3, "'### Two families...' is level 3");
        Expect(!headings[3].Number.has_value(), "'### Two families...' is unnumbered");
        Expect(!headings[8].Number.has_value(), "'# §9b.' does not match the strict numbered form");
        ExpectEq(headings[8].Title, "\xC2\xA7" "9b. The VERTICAL axis closed: `Alignment.center`",
                "'# §9b.' keeps its raw text as Title");
    }
}

int main()
{
    TestHeadingParsing();
    return Finish();
}
