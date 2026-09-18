// Sherlock — tests/Sherlock/DocumentLayerGates.cpp
// Corpus-free gates for q's layer-3 blocks (PrintDocumentLayer) and status's layer-3 line: both
// must degrade to a stated zero, never a crash, when Documents.db is absent or schema-mismatched
// (decision 4), and status's staleness counts must reflect what Tasks 6-7 actually wrote (a
// per-file Size/MTime stamp, not a HEAD-or-file-count compare).
#include <SherlockCli/Queries.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include "SherlockHarness.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace Sherlock;

namespace
{
    std::int64_t MTime(const std::filesystem::path& path)
    {
        return std::filesystem::last_write_time(path).time_since_epoch().count();
    }

    struct Fixture
    {
        std::filesystem::path Root;
        std::filesystem::path Store;
        std::filesystem::path Documents;
    };

    Fixture CreateFixture()
    {
        Fixture fixture;
        fixture.Root = std::filesystem::temp_directory_path() / "SherlockDocumentLayerGate";
        std::filesystem::remove_all(fixture.Root);
        std::filesystem::create_directories(fixture.Root / "docs" / "re");
        fixture.Store = fixture.Root / "Store";
        std::filesystem::create_directories(fixture.Store);
        fixture.Documents = fixture.Root / "Documents.db";

        // RunStatus opens this Catalog.db regardless of layer 3 -- zero images is a legitimate,
        // already-exercised shape (LoadImages returns an empty vector, not an error).
        auto catalog = Store::Database::Open(fixture.Store / "Catalog.db", Store::Database::Mode::ReadWrite);
        Expect(catalog.has_value(), "the catalog fixture opens");
        if (catalog)
        {
            Expect(Store::CreateCatalog(*catalog, "test-build", "test-cache-uuid").has_value(),
                   "the catalog fixture uses the production schema API");
        }

        const auto laudo = fixture.Root / "docs" / "re" / "sample.md";
        {
            std::ofstream stream(laudo, std::ios::binary);
            stream << "## \xC2\xA7"
                      "7 LayerResolver\r\n\r\n100% shadow pool.\r\n";
        }

        auto documents = Store::Database::Open(fixture.Documents, Store::Database::Mode::ReadWrite);
        Expect(documents.has_value(), "the documents fixture opens");
        if (!documents)
        {
            return fixture;
        }
        Expect(Store::CreateDocumentsStore(*documents).has_value() &&
                   Store::CreateDocumentIndexes(*documents).has_value(),
               "the documents fixture uses the production schema APIs");
        Expect(documents->Execute("INSERT INTO Meta(Key, Value) VALUES('Head', "
                                  "'deadbeefcafebabe0000000000000000000000')").has_value(),
               "the fixture records a Head");
        Expect(documents->Execute("INSERT INTO Coverage(Root, Read, Total) VALUES('docs/re', 1, 1)").has_value(),
               "the fixture records docs/re coverage");

        auto insertFile = documents->Prepare("INSERT INTO File(Path, Size, MTime) VALUES(?1, ?2, ?3)");
        Expect(insertFile.has_value(), "the fixture File insert prepares");
        if (insertFile)
        {
            Expect(insertFile->Bind(1, std::string_view("docs/re/sample.md")).has_value() &&
                       insertFile->Bind(2, static_cast<std::int64_t>(std::filesystem::file_size(laudo))).has_value() &&
                       insertFile->Bind(3, MTime(laudo)).has_value() && insertFile->Step().has_value(),
                   "the fixture records its backing file stamp");
        }

        auto insertSection = documents->Prepare(
            "INSERT INTO Section(File, Number, Title, FirstLine, LastLine, Text) "
            "VALUES('docs/re/sample.md', '7', 'LayerResolver', 1, 3, 'placeholder')");
        Expect(insertSection.has_value() && insertSection->Step().has_value(), "the fixture inserts one section");
        const auto sectionId = documents->LastInsertId();

        auto insertCitation = documents->Prepare("INSERT INTO Citation(Section, Address, Symbol) VALUES(?1, ?2, NULL)");
        Expect(insertCitation.has_value(), "the citation insert prepares");
        if (insertCitation)
        {
            Expect(insertCitation->Bind(1, sectionId).has_value() &&
                       insertCitation->Bind(2, static_cast<std::int64_t>(0x27c198c20)).has_value() &&
                       insertCitation->Step().has_value(),
                   "the fixture inserts one citation");
        }

        auto insertSeal = documents->Prepare(
            "INSERT INTO Seal(File, Line, Tag, Image, Symbol, Address) VALUES(?1, 6, 'BIN', ?2, NULL, ?3)");
        Expect(insertSeal.has_value(), "the seal insert prepares");
        if (insertSeal)
        {
            Expect(insertSeal->Bind(1, std::string_view("Source/AgentCanvasKit/SnippetSizeConstants.h")).has_value() &&
                       insertSeal->Bind(2, std::string_view("AgentCanvasKit")).has_value() &&
                       insertSeal->Bind(3, static_cast<std::int64_t>(0x27c198c20)).has_value() &&
                       insertSeal->Step().has_value(),
                   "the fixture inserts one seal");
        }
        return fixture;
    }

    Cli::QueryEnvironment Environment(const Fixture& fixture, std::vector<std::string>& output, bool withDocuments)
    {
        Cli::QueryEnvironment env;
        env.Store     = fixture.Store;
        env.Documents = withDocuments ? fixture.Documents : std::filesystem::path{};
        env.Repo      = fixture.Root;
        env.Json      = true; // Emit() still fills Output without printing to stdout
        env.Output    = &output;
        return env;
    }

    void GateStatusWithoutDocuments(const Fixture& fixture)
    {
        std::vector<std::string> output;
        const auto                verdict = Cli::RunStatus(Environment(fixture, output, false));
        Expect(verdict.Kind == Cli::VerdictKind::Found, "status still answers when Documents.db is absent");
        bool sawNotBuilt = false, sawSectionCount = false;
        for (const auto& line : output)
        {
            if (line == "layer 3: not built") sawNotBuilt = true;
            if (line.find("section(s)") != std::string::npos) sawSectionCount = true;
        }
        Expect(sawNotBuilt, "status reports layer 3 as not built when Documents.db is absent");
        Expect(!sawSectionCount, "status never fabricates a section count when Documents.db is absent");
    }

    void GateQueryLayerNotBuilt(const Fixture& fixture)
    {
        std::vector<std::string> output;
        Cli::PrintDocumentLayer(Environment(fixture, output, false), 0x27c198c20);
        Expect(output.size() == 1 && output.front() == "layer 3: not built",
               "q's layer 3 block prints exactly one line when Documents.db is absent, nothing else");
    }

    void GateQueryLayerCitedAndSealed(const Fixture& fixture)
    {
        std::vector<std::string> output;
        Cli::PrintDocumentLayer(Environment(fixture, output, true), 0x27c198c20);
        bool sawCitedLine = false, sawCitedCount = false, sawSealedLine = false, sawSealedCount = false;
        for (const auto& line : output)
        {
            if (line.find("cited by sample.md") != std::string::npos && line.find("LayerResolver") != std::string::npos)
                sawCitedLine = true;
            if (line == "  cited by: 1") sawCitedCount = true;
            if (line.find("sealed at Source/AgentCanvasKit/SnippetSizeConstants.h:6") != std::string::npos &&
                line.find("BIN") != std::string::npos && line.find("AgentCanvasKit") != std::string::npos)
                sawSealedLine = true;
            if (line == "  sealed at: 1") sawSealedCount = true;
        }
        Expect(sawCitedLine, "q prints the section that cites the resolved address");
        Expect(sawCitedCount, "q's cited-by summary counts exactly the rows it printed");
        Expect(sawSealedLine, "q prints the seal at the resolved address");
        Expect(sawSealedCount, "q's sealed-at summary counts exactly the rows it printed");

        // Gate strength: an address nothing cites or seals must report zero, not leak the fixture's
        // one row -- a gate that only checked "some cited by line appears" would survive a bug
        // that ignores the bound address entirely.
        std::vector<std::string> missOutput;
        Cli::PrintDocumentLayer(Environment(fixture, missOutput, true), 0x1);
        bool sawZeroCited = false, sawZeroSealed = false, sawLeakedLine = false;
        for (const auto& line : missOutput)
        {
            if (line == "  cited by: 0") sawZeroCited = true;
            if (line == "  sealed at: 0") sawZeroSealed = true;
            if (line.find("LayerResolver") != std::string::npos || line.find("SnippetSizeConstants") != std::string::npos)
                sawLeakedLine = true;
        }
        Expect(sawZeroCited, "q reports zero citations for an address nothing cites");
        Expect(sawZeroSealed, "q reports zero seals for an address nothing seals");
        Expect(!sawLeakedLine, "q never prints another address's citation or seal");
    }

    void GateQueryLayerTruncation(const Fixture& fixture)
    {
        // A dedicated Documents.db (not the shared fixture) with 12 citations under one address --
        // proves the default 10-row cap AND that --full lifts it, without disturbing the counts
        // the other gates depend on.
        const auto path = fixture.Root / "TruncationDocuments.db";
        auto       db   = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "the truncation fixture opens");
        if (!db) return;
        Expect(Store::CreateDocumentsStore(*db).has_value() && Store::CreateDocumentIndexes(*db).has_value(),
               "the truncation fixture uses the production schema APIs");
        for (int i = 0; i < 12; ++i)
        {
            auto section = db->Prepare(std::format(
                "INSERT INTO Section(File, Number, Title, FirstLine, LastLine, Text) "
                "VALUES('docs/re/many.md', '{}', 'Row {}', 1, 1, 'x')", i, i));
            Expect(section.has_value() && section->Step().has_value(), "the truncation fixture inserts one section");
            const auto sectionId = db->LastInsertId();
            auto       citation  = db->Prepare("INSERT INTO Citation(Section, Address, Symbol) VALUES(?1, ?2, NULL)");
            Expect(citation.has_value() && citation->Bind(1, sectionId).has_value() &&
                       citation->Bind(2, static_cast<std::int64_t>(0x42)).has_value() && citation->Step().has_value(),
                   "the truncation fixture inserts one citation");
        }

        Fixture truncation   = fixture;
        truncation.Documents = path;

        std::vector<std::string> defaultOutput;
        Cli::PrintDocumentLayer(Environment(truncation, defaultOutput, true), 0x42);
        std::size_t citedLines = 0;
        for (const auto& line : defaultOutput)
        {
            if (line.starts_with("  cited by ") && line.find(" \xE2\x80\x94 ") != std::string::npos) ++citedLines;
        }
        Expect(citedLines == 10, "q prints at most 10 cited-by rows without --full");
        Expect(std::find(defaultOutput.begin(), defaultOutput.end(), "  cited by: 12") != defaultOutput.end(),
               "q's cited-by summary still counts all 12 rows even when the list is truncated");

        std::vector<std::string> fullOutput;
        auto                      env = Environment(truncation, fullOutput, true);
        env.Full                      = true;
        Cli::PrintDocumentLayer(env, 0x42);
        std::size_t fullCitedLines = 0;
        for (const auto& line : fullOutput)
        {
            if (line.starts_with("  cited by ") && line.find(" \xE2\x80\x94 ") != std::string::npos) ++fullCitedLines;
        }
        Expect(fullCitedLines == 12, "--full lifts the 10-row cap and prints every cited-by row");
    }

    void GateSchemaMismatch(const Fixture& fixture)
    {
        // A private copy, so the other gates' fixture is untouched (mirrors DocumentsGates.cpp's
        // own GateDocumentsSchemaRefusal).
        const auto corruptPath = fixture.Root / "CorruptDocuments.db";
        std::filesystem::copy_file(fixture.Documents, corruptPath, std::filesystem::copy_options::overwrite_existing);
        {
            auto db = Store::Database::Open(corruptPath, Store::Database::Mode::ReadWrite);
            Expect(db.has_value() && db->Execute("UPDATE Meta SET Value = '0' WHERE Key = 'SchemaVersion'").has_value(),
                   "the fixture corrupts the documents schema metadata");
        }
        Fixture corrupt   = fixture;
        corrupt.Documents = corruptPath;

        std::vector<std::string> output;
        Cli::PrintDocumentLayer(Environment(corrupt, output, true), 0x27c198c20);
        Expect(output.size() == 1 && output.front() == "layer 3: not built",
               "q refuses a Documents.db with the wrong schema version instead of reading it");

        std::vector<std::string> statusOutput;
        const auto                verdict = Cli::RunStatus(Environment(corrupt, statusOutput, true));
        bool sawNotBuilt = false, sawSectionCount = false;
        for (const auto& line : statusOutput)
        {
            if (line == "layer 3: not built") sawNotBuilt = true;
            if (line.find("section(s)") != std::string::npos) sawSectionCount = true;
        }
        Expect(verdict.Kind == Cli::VerdictKind::Found, "status still answers when Documents.db has the wrong schema");
        Expect(sawNotBuilt, "status refuses a mismatched Documents.db schema instead of reading it");
        Expect(!sawSectionCount, "status never reports counts read from a schema-mismatched store");
    }

    void GateStatusWithDocuments(const Fixture& fixture)
    {
        std::vector<std::string> output;
        const auto                verdict = Cli::RunStatus(Environment(fixture, output, true));
        Expect(verdict.Kind == Cli::VerdictKind::Found, "status answers with Documents.db present");
        bool sawLayerThree = false, sawHead = false, sawCounts = false;
        for (const auto& line : output)
        {
            if (line == "layer 3: 1 section(s), 1 citation(s), 1 seal(s), coverage 1/1 file(s)") sawLayerThree = true;
            if (line.find("head: deadbeefcafebabe0000000000000000000000") != std::string::npos) sawHead = true;
            if (line == "changed: 0, added: 0, removed: 0 (since this store was built)") sawCounts = true;
        }
        Expect(sawLayerThree, "status prints the exact section/citation/seal/coverage counts");
        Expect(sawHead, "status prints Head as information, not as the staleness check");
        Expect(sawCounts, "status reports zero changed/added/removed against an untouched fixture");
    }

    void GateStatusDetectsChangedAndAdded(const Fixture& fixture)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::ofstream stream(fixture.Root / "docs" / "re" / "sample.md", std::ios::binary | std::ios::app);
            stream << "changed\r\n";
        }
        const auto extra = fixture.Root / "docs" / "re" / "extra.md";
        {
            std::ofstream stream(extra, std::ios::binary);
            stream << "# New\r\n";
        }

        std::vector<std::string> output;
        const auto                verdict = Cli::RunStatus(Environment(fixture, output, true));
        Expect(verdict.Kind == Cli::VerdictKind::Found, "status still answers once files changed underneath it");
        Expect(std::find(output.begin(), output.end(), "changed: 1, added: 1, removed: 0 (since this store was built)") !=
                   output.end(),
               "status counts the edited file as changed and the new file as added");
        std::filesystem::remove(extra);
    }

    void GateStatusDetectsRemoved(const Fixture& fixture)
    {
        std::filesystem::remove(fixture.Root / "docs" / "re" / "sample.md");
        std::vector<std::string> output;
        const auto                verdict = Cli::RunStatus(Environment(fixture, output, true));
        Expect(verdict.Kind == Cli::VerdictKind::Found, "status still answers once a backing file is gone");
        Expect(std::find(output.begin(), output.end(), "changed: 0, added: 0, removed: 1 (since this store was built)") !=
                   output.end(),
               "status counts a deleted file as removed, not changed");
    }
}

int main()
{
    const auto fixture = CreateFixture();
    GateStatusWithoutDocuments(fixture);
    GateQueryLayerNotBuilt(fixture);
    GateQueryLayerCitedAndSealed(fixture);
    GateQueryLayerTruncation(fixture);
    GateSchemaMismatch(fixture);
    GateStatusWithDocuments(fixture);
    GateStatusDetectsChangedAndAdded(fixture);
    GateStatusDetectsRemoved(fixture);
    std::filesystem::remove_all(fixture.Root);
    return Finish();
}
