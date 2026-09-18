// Sherlock — tests/Sherlock/DocumentIndexGates.cpp
// Unit gates for DocumentIndex's pure parsers, over the fixtures in tests/Sherlock/fixtures/.
#include "SherlockHarness.h"

#include <DocumentIndex/CitationExtractor.h>
#include <DocumentIndex/Builder.h>
#include <DocumentIndex/FileStamp.h>
#include <DocumentIndex/GitHead.h>
#include <DocumentIndex/Heading.h>
#include <DocumentIndex/LaudoSections.h>
#include <DocumentIndex/SealExtractor.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace Sherlock;

namespace
{
    std::filesystem::path FixturePath(const char* name)
    {
        return std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    }

    std::string ReadFixture(const char* name)
    {
        std::ifstream stream(FixturePath(name));
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

    void TestSplitDocumentLaudo()
    {
        auto sections = DocumentIndex::SplitDocument(FixturePath("heading-sample.md"), "docs/re/heading-sample.md");
        Expect(sections.has_value(), "SplitDocument parses the heading fixture");
        const auto& list = *sections;
        const auto  types =
            std::find_if(list.begin(), list.end(), [](const auto& s) { return s.Number == "1"; });
        Expect(types != list.end(), "there is a Number==\"1\" section");
        Expect(types->Text.find("BannerCompositionContent") != std::string::npos,
              "the nested ### blocks stay inside the enclosing ## section's Text");
        Expect(types->Text.find("Why this framework") == std::string::npos,
              "the ## 1. section stops before its same-level sibling '## Why this framework'");
        const auto recipe = std::find_if(list.begin(), list.end(), [](const auto& s) {
            return s.Title.find("BannerCompositionContent") != std::string::npos;
        });
        Expect(recipe != list.end(), "the nested block is ALSO its own addressable section");
        ExpectEq(recipe->Number.has_value(), false, "the nested block is unnumbered");
        Expect(recipe->Text.find("BannerSlider") == std::string::npos,
              "the nested ### recipe section stops before its own same-level sibling ### BannerSlider.Recipe");
    }

    void TestSplitDocumentKeepsSourceBytes()
    {
        const auto root = std::filesystem::temp_directory_path() / "SherlockDocumentSliceGate";
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        std::filesystem::create_directories(root);
        const auto file = root / "sample.md";
        {
            std::ofstream stream(file, std::ios::binary);
            stream << "# 1 First\r\nbody\r\n# 2 Second\r\nend\r\n";
        }
        const auto sections = DocumentIndex::SplitDocument(file, "docs/re/sample.md");
        Expect(sections.has_value() && sections->size() == 2, "SplitDocument reads two CRLF sections");
        if (sections && sections->size() == 2)
        {
            ExpectEq((*sections)[0].Text, std::string("# 1 First\r\nbody\r\n"),
                     "a section keeps its original CRLF bytes through the next peer heading");
        }
        std::filesystem::remove_all(root, cleanupError);
    }

    void TestReadCurrentHead()
    {
        const auto root = std::filesystem::temp_directory_path() / "SherlockGitHeadGate";
        const std::string sha(40, 'a');
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        std::filesystem::create_directories(root / ".git" / "refs" / "heads");
        {
            std::ofstream head(root / ".git" / "HEAD");
            head << "ref: refs/heads/x\n";
            std::ofstream ref(root / ".git" / "refs" / "heads" / "x");
            ref << sha << "\n";
        }
        const auto loose = DocumentIndex::ReadCurrentHead(root);
        Expect(loose.has_value() && *loose == sha, "ReadCurrentHead follows a loose ref");
        {
            std::ofstream head(root / ".git" / "HEAD");
            head << sha << "\n";
        }
        const auto detached = DocumentIndex::ReadCurrentHead(root);
        Expect(detached.has_value() && *detached == sha, "ReadCurrentHead returns a detached head");
        std::filesystem::remove(root / ".git" / "refs" / "heads" / "x", cleanupError);
        {
            std::ofstream head(root / ".git" / "HEAD");
            head << "ref: refs/heads/x\n";
            std::ofstream packedFile(root / ".git" / "packed-refs");
            packedFile << "# pack-refs with: peeled fully-peeled\n" << sha << " refs/heads/x\n";
        }
        const auto packed = DocumentIndex::ReadCurrentHead(root);
        Expect(packed.has_value() && *packed == sha, "ReadCurrentHead falls back to packed refs");

        const auto gitdir = root / "linked-gitdir";
        std::filesystem::create_directories(gitdir / "refs" / "heads");
        {
            std::ofstream dotGit(root / ".git");
            dotGit << "gitdir: " << gitdir.string() << "\n";
            std::ofstream head(gitdir / "HEAD");
            head << "ref: refs/heads/x\n";
            std::ofstream ref(gitdir / "refs" / "heads" / "x");
            ref << sha << "\n";
        }
        const auto linkedLoose = DocumentIndex::ReadCurrentHead(root);
        Expect(linkedLoose.has_value() && *linkedLoose == sha, "ReadCurrentHead follows a linked-worktree loose ref");
        std::filesystem::remove(gitdir / "refs" / "heads" / "x", cleanupError);
        {
            std::ofstream linkedPackedFile(gitdir / "packed-refs");
            linkedPackedFile << sha << " refs/heads/x\n";
        }
        const auto linkedPacked = DocumentIndex::ReadCurrentHead(root);
        Expect(linkedPacked.has_value() && *linkedPacked == sha,
               "ReadCurrentHead follows a linked-worktree packed ref");
        const auto common = root / "common-gitdir";
        std::filesystem::create_directories(common / "refs" / "heads");
        {
            std::ofstream commonDir(gitdir / "commondir");
            commonDir << "../common-gitdir\n";
            std::ofstream commonRef(common / "refs" / "heads" / "x");
            commonRef << sha << "\n";
            std::filesystem::remove(gitdir / "packed-refs", cleanupError);
        }
        const auto linkedCommonLoose = DocumentIndex::ReadCurrentHead(root);
        Expect(linkedCommonLoose.has_value() && *linkedCommonLoose == sha,
               "ReadCurrentHead follows a linked-worktree common loose ref");
        std::filesystem::remove(common / "refs" / "heads" / "x", cleanupError);
        {
            std::ofstream commonPacked(common / "packed-refs");
            commonPacked << sha << " refs/heads/x\n";
        }
        const auto linkedCommonPacked = DocumentIndex::ReadCurrentHead(root);
        Expect(linkedCommonPacked.has_value() && *linkedCommonPacked == sha,
               "ReadCurrentHead follows a linked-worktree common packed ref");
        std::filesystem::remove_all(root, cleanupError);
    }

    void TestBuildDocumentsWritesCoverageFilesAndMeta()
    {
        const auto root = std::filesystem::temp_directory_path() / "SherlockBuilderGate";
        const auto documents = root / "out" / "Documents.db";
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        std::filesystem::create_directories(root / ".git");
        std::filesystem::create_directories(root / "docs" / "re");
        std::filesystem::create_directories(root / "docs" / "concepts");
        std::filesystem::create_directories(root / "Source");
        {
            std::ofstream head(root / ".git" / "HEAD");
            head << std::string(40, 'b') << "\n";
            std::ofstream re(root / "docs" / "re" / "finding.md", std::ios::binary);
            re << "# 1 Finding\nAddress 0x27c198c20\n";
            std::ofstream conceptFile(root / "docs" / "concepts" / "colour.md", std::ios::binary);
            conceptFile << "---\ntitle: Colour\naliases: [0x27c198c20, Glass.Material]\n---\nProse\n";
            std::ofstream source(root / "Source" / "Seal.h", std::ios::binary);
            source << "// [BIN] DesignLibrary 0x27c198c20\n";
        }
        const auto report = DocumentIndex::BuildDocuments(root, documents);
        Expect(report.has_value(), "BuildDocuments indexes a minimal repository");
        if (report)
        {
            ExpectEq(report->LaudoFilesRead, std::uint64_t{1}, "the laudo coverage read count");
            ExpectEq(report->ConceptFilesRead, std::uint64_t{1}, "the concept coverage read count");
            ExpectEq(report->SourceFilesRead, std::uint64_t{1}, "the source coverage read count");
        }
        auto db = Store::Database::Open(documents, Store::Database::Mode::ReadOnly);
        Expect(db.has_value(), "the built documents store can be opened");
        if (db)
        {
            const auto sourceCoverage = db->ScalarInt("SELECT Read FROM Coverage WHERE Root = 'Source'");
            const auto files = db->ScalarInt("SELECT COUNT(*) FROM File");
            const auto head = Store::ReadMeta(*db, "Head");
            Expect(sourceCoverage.has_value() && *sourceCoverage == 1,
                   "Coverage records the source file actually indexed");
            Expect(files.has_value() && *files == 3, "File records every successfully indexed input");
            Expect(head.has_value() && *head == std::string(40, 'b'), "Meta records the current head as information");
        }
        std::filesystem::remove_all(root, cleanupError);
    }

    void TestBuildDocumentsRefusesDirectoryAsDatabasePath()
    {
        const auto root = std::filesystem::temp_directory_path() / "SherlockBuilderDirectoryGate";
        const auto documents = root / "out" / "Documents.db";
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        std::filesystem::create_directories(documents);
        const auto report = DocumentIndex::BuildDocuments(root, documents);
        Expect(!report.has_value(), "BuildDocuments refuses a directory passed as --documents");
        Expect(std::filesystem::is_directory(documents), "BuildDocuments leaves a --documents directory intact");
        std::filesystem::remove_all(root, cleanupError);
    }

    void TestBuildDocumentsRollsBackSchemaAfterInputFailure()
    {
        const auto root = std::filesystem::temp_directory_path() / "SherlockBuilderRollbackGate";
        const auto documents = root / "out" / "Documents.db";
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        std::filesystem::create_directories(root / "docs" / "concepts");
        {
            std::ofstream invalid(root / "docs" / "concepts" / "invalid.md", std::ios::binary);
            invalid << "---\ntitle: missing closing front matter\n";
        }
        const auto report = DocumentIndex::BuildDocuments(root, documents);
        Expect(!report.has_value(), "a malformed indexed document fails the build");
        auto db = Store::Database::Open(documents, Store::Database::Mode::ReadOnly);
        if (db)
        {
            const auto schema = Store::CheckDocumentsSchema(*db);
            Expect(!schema.has_value(), "a failed build leaves no valid Documents schema or metadata");
        }
        else
        {
            Expect(true, "a failed build leaves no readable Documents database");
        }
        std::filesystem::remove_all(root, cleanupError);
    }

    void TestSplitDocumentConcept()
    {
        auto sections =
            DocumentIndex::SplitDocument(FixturePath("concept-sample.md"), "docs/concepts/concept-sample.md");
        Expect(sections.has_value(), "SplitDocument parses the concept fixture");
        ExpectEq(sections->size(), std::size_t(1), "a concept page is exactly one Section");
        ExpectEq((*sections)[0].Title, "The colour-matrix product", "Title comes from the front matter");
    }

    void TestCitationExtraction()
    {
        const auto text      = ReadFixture("citation-sample.md");
        const auto citations = DocumentIndex::ExtractCitations(text);
        std::size_t addresses = 0, symbols = 0;
        bool sawTargetAddress = false, sawShortHexAsAddress = false;
        bool sawMangled = false, sawDottedSymbol = false, sawSecondDottedSymbol = false;
        bool sawFileNameAsSymbol = false, sawPathAsSymbol = false, sawBareWordAsSymbol = false;
        for (const auto& citation : citations)
        {
            if (citation.Address)
            {
                ++addresses;
                sawTargetAddress     = sawTargetAddress || *citation.Address == 0x27c198c20ull;
                sawShortHexAsAddress = sawShortHexAsAddress || *citation.Address == 0x1a2bull;
            }
            if (citation.Symbol)
            {
                ++symbols;
                sawMangled             = sawMangled || citation.Symbol->starts_with("_$s");
                sawDottedSymbol        = sawDottedSymbol || *citation.Symbol == "GlassMaterialProvider.Configuration";
                sawSecondDottedSymbol  = sawSecondDottedSymbol ||
                                         *citation.Symbol == "GlassEdgeMaterialProvider.resolveLayers";
                sawFileNameAsSymbol    = sawFileNameAsSymbol || *citation.Symbol == "WindowControlColors.h";
                sawPathAsSymbol        = sawPathAsSymbol || *citation.Symbol == "Assets/Icons.Bundle";
                sawBareWordAsSymbol    = sawBareWordAsSymbol || *citation.Symbol == "DesignLibrary" ||
                                         *citation.Symbol == "DarkShadow";
            }
        }
        ExpectEq(addresses, std::size_t(1),
                "0x27c198c20 appears twice in prose, once bare, and dedupes to one Address; the short "
                "hex-ish 0x1a2b token never qualifies");
        Expect(sawTargetAddress, "the deduped address is the one the fixture cites");
        Expect(!sawShortHexAsAddress, "0x1a2b has too few hex digits to become an Address citation");
        ExpectEq(symbols, std::size_t(3),
                "extraction closes on exactly the three real symbols: two dotted chains and the "
                "mangled name -- a file name, a path and a dotted mention repeated twice never add up");
        Expect(sawMangled, "the mangled _$s... token becomes a Symbol");
        Expect(sawDottedSymbol, "a dotted Apple-style chain becomes a Symbol");
        Expect(sawSecondDottedSymbol, "a second, distinct dotted chain also becomes a Symbol");
        Expect(!sawFileNameAsSymbol, "WindowControlColors.h is excluded as a file name, not a symbol");
        Expect(!sawPathAsSymbol, "Assets/Icons.Bundle is excluded as a path, not a symbol");
        Expect(!sawBareWordAsSymbol, "a dotless backtick word is never mistaken for a dotted symbol");
    }

    void TestSealExtraction()
    {
        auto seals = DocumentIndex::ExtractSeals(FixturePath("seal-sample.txt"),
                                                 "tests/Sherlock/fixtures/seal-sample.txt");
        Expect(seals.has_value(), "ExtractSeals reads the fixture");
        const auto& rows = *seals;
        ExpectEq(rows.size(), std::size_t(7),
                "seven seals: the string-literal '[BIN]' is never scanned (no comment block backs it)");

        ExpectEq(rows[0].Tag, "BIN", "first row is [BIN]");
        ExpectEq(rows[0].Image.value_or(""), "AgentCanvasKit", "its image");
        Expect(rows[0].Address.has_value() && *rows[0].Address == 0x22695fe48ull,
              "the first address in the segment, matching the clean control");

        ExpectEq(rows[1].Tag, "BIN", "second row is [BIN] DesignLibrary");
        ExpectEq(rows[1].Image.value_or(""), "DesignLibrary", "its image");
        Expect(!rows[1].Address.has_value(),
              "no-address: 0x27c198c20 sits BEFORE the tag, matching tools/seals-baseline.txt's own verdict");

        ExpectEq(rows[2].Tag, "KIT", "third row is a trailing-comment [KIT] seal");
        Expect(!rows[2].Image.has_value(),
              "lint_seals.py's BIN regex is the only one that captures an image -- [KIT] never gets one, "
              "so the word right after the tag ('Figma') must not leak in as an Image");
        Expect(!rows[2].Address.has_value(), "no hex in this seal's prose");

        ExpectEq(rows[3].Tag, "API", "fourth row is [API]");
        Expect(!rows[3].Image.has_value(), "[API] never captures an image either");
        Expect(!rows[3].Address.has_value(), "[API] with genuinely no address stays no-address");

        ExpectEq(rows[4].Tag, "OBS", "fifth row is [OBS]");
        Expect(!rows[4].Address.has_value(),
              "0x100002710 is ROOTFS-shaped (0x10...), not cache-shaped -- an address-looking hex "
              "outside the cache range must never become an Address citation");

        ExpectEq(rows[5].Tag, "BIN", "sixth row is the glued-continuation [BIN]");
        ExpectEq(rows[5].Image.value_or(""), "GlueTest", "its image");
        Expect(rows[5].Address.has_value() && *rows[5].Address == 0x18a000030ull,
              "the address sits on the NEXT line, glued to this one only because the continuation "
              "offset is measured on the line's trimmed text, not its raw (still-indented) one");

        ExpectEq(rows[6].Tag, "BIN", "seventh row is a second glued-continuation [BIN]");
        ExpectEq(rows[6].Image.value_or(""), "GlueTest2", "its image");
        Expect(rows[6].Address.has_value() && *rows[6].Address == 0x18a000040ull,
              "this continuation's trimmed offset is 5 (between 4 and 8) -- a gate whose threshold "
              "check tolerates 8->4 without noticing must still catch this one");

        for (const auto& row : rows)
        {
            Expect(!row.Symbol.has_value(), "Seal.Symbol stays null in phase 2 (decision 5)");
        }
    }
}

int main()
{
    TestHeadingParsing();
    TestSplitDocumentLaudo();
    TestSplitDocumentKeepsSourceBytes();
    TestSplitDocumentConcept();
    TestReadCurrentHead();
    TestBuildDocumentsWritesCoverageFilesAndMeta();
    TestBuildDocumentsRefusesDirectoryAsDatabasePath();
    TestBuildDocumentsRollsBackSchemaAfterInputFailure();
    TestCitationExtraction();
    TestSealExtraction();
    return Finish();
}
