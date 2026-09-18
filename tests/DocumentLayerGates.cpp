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
        bool sawNotBuilt = false, sawBuildCommand = false, sawSectionCount = false;
        for (const auto& line : output)
        {
            if (line.starts_with("layer 3: not built")) sawNotBuilt = true;
            if (line.find("Sherlock build docs --repo") != std::string::npos) sawBuildCommand = true;
            if (line.find("section(s)") != std::string::npos) sawSectionCount = true;
        }
        Expect(sawNotBuilt, "status reports layer 3 as not built when Documents.db is absent");
        Expect(sawBuildCommand,
               "status's not-built line names the exact command that builds layer 3, not just the fact");
        Expect(!sawSectionCount, "status never fabricates a section count when Documents.db is absent");
    }

    void GateQueryLayerNotBuilt(const Fixture& fixture)
    {
        std::vector<std::string> output;
        Cli::PrintDocumentLayer(Environment(fixture, output, false), 0x27c198c20);
        Expect(output.size() == 1 && output.front().starts_with("layer 3: not built"),
               "q's layer 3 block prints exactly one line when Documents.db is absent, nothing else");
        Expect(!output.empty() && output.front().find("Sherlock build docs --repo") != std::string::npos,
               "q's not-built line names the exact command that builds layer 3 -- a message that "
               "names the fix instead of an agent hunting for a Documents.db path");
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
        Expect(output.size() == 1 && output.front().starts_with("layer 3: not built"),
               "q refuses a Documents.db with the wrong schema version instead of reading it");

        std::vector<std::string> statusOutput;
        const auto                verdict = Cli::RunStatus(Environment(corrupt, statusOutput, true));
        bool sawNotBuilt = false, sawSectionCount = false;
        for (const auto& line : statusOutput)
        {
            if (line.starts_with("layer 3: not built")) sawNotBuilt = true;
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

    // A Prepare/Bind failure would otherwise make the whole "cited by"/"sealed at" line vanish,
    // and a mid-loop Step() failure reads as end-of-rows, silently truncating the count -- both
    // are the instrument reporting a fact ("N") where it cannot look. A private copy per broken
    // table, so the other gates' fixture is untouched (finding 3).
    void GateCitedByAndSealedAtPropagateFailure(const Fixture& fixture)
    {
        {
            const auto brokenPath = fixture.Root / "NoCitationDocuments.db";
            std::filesystem::copy_file(fixture.Documents, brokenPath, std::filesystem::copy_options::overwrite_existing);
            {
                auto db = Store::Database::Open(brokenPath, Store::Database::Mode::ReadWrite);
                Expect(db.has_value() && db->Execute("DROP TABLE Citation").has_value(),
                       "the fixture drops the Citation table");
            }
            Fixture broken   = fixture;
            broken.Documents = brokenPath;

            std::vector<std::string> output;
            Cli::PrintDocumentLayer(Environment(broken, output, true), 0x27c198c20);
            bool sawCitedNotVerified = false, sawSealedCount = false, sawFabricatedZero = false;
            for (const auto& line : output)
            {
                if (line.starts_with("  cited by: NOT VERIFIED")) sawCitedNotVerified = true;
                if (line == "  cited by: 0") sawFabricatedZero = true;
                if (line == "  sealed at: 1") sawSealedCount = true;
            }
            Expect(sawCitedNotVerified, "q reports the cited-by line as NOT VERIFIED when Citation cannot be queried");
            Expect(!sawFabricatedZero, "q never reports a fabricated zero cited-by count for a query it could not run");
            Expect(sawSealedCount, "the sealed-at block still answers normally -- only the broken table is affected");
        }
        {
            const auto brokenPath = fixture.Root / "NoSealDocuments.db";
            std::filesystem::copy_file(fixture.Documents, brokenPath, std::filesystem::copy_options::overwrite_existing);
            {
                auto db = Store::Database::Open(brokenPath, Store::Database::Mode::ReadWrite);
                Expect(db.has_value() && db->Execute("DROP TABLE Seal").has_value(), "the fixture drops the Seal table");
            }
            Fixture broken   = fixture;
            broken.Documents = brokenPath;

            std::vector<std::string> output;
            Cli::PrintDocumentLayer(Environment(broken, output, true), 0x27c198c20);
            bool sawSealedNotVerified = false, sawCitedCount = false;
            for (const auto& line : output)
            {
                if (line.starts_with("  sealed at: NOT VERIFIED")) sawSealedNotVerified = true;
                if (line == "  cited by: 1") sawCitedCount = true;
            }
            Expect(sawSealedNotVerified, "q reports the sealed-at line as NOT VERIFIED when Seal cannot be queried");
            Expect(sawCitedCount, "the cited-by block still answers normally -- only the broken table is affected");
        }
    }

    // A Documents.db that exists but cannot actually be read (corrupt bytes, wrong format) is a
    // different fact than "never built" -- folding both into the same "not built" line is the
    // instrument lying about what it could establish (finding 4).
    void GateDocumentsUnreadableIsNotVerified(const Fixture& fixture)
    {
        const auto garbagePath = fixture.Root / "GarbageDocuments.db";
        {
            std::ofstream stream(garbagePath, std::ios::binary);
            stream << "this is not a SQLite database file, just some bytes\n";
        }
        Fixture garbage   = fixture;
        garbage.Documents = garbagePath;

        std::vector<std::string> output;
        Cli::PrintDocumentLayer(Environment(garbage, output, true), 0x27c198c20);
        Expect(output.size() == 1 && output.front().starts_with("layer 3: NOT VERIFIED"),
               "q reports an unreadable Documents.db as NOT VERIFIED, not as \"not built\"");

        std::vector<std::string> statusOutput;
        const auto                verdict = Cli::RunStatus(Environment(garbage, statusOutput, true));
        Expect(verdict.Kind == Cli::VerdictKind::Found,
               "status's own layer-1/2 answer still stands when only layer 3 is unreadable");
        bool sawNotVerified = false, sawNotBuilt = false;
        for (const auto& line : statusOutput)
        {
            if (line.starts_with("layer 3: NOT VERIFIED")) sawNotVerified = true;
            if (line == "layer 3: not built") sawNotBuilt = true;
        }
        Expect(sawNotVerified, "status reports an unreadable Documents.db as NOT VERIFIED, not as \"not built\"");
        Expect(!sawNotBuilt, "status never calls an unreadable store \"not built\"");
    }

    // A Prepare/Step failure mid-scan would otherwise leave `known` short while the walk still
    // runs to completion, printing a confident "changed: 0, added: 0, removed: 0" about a scan
    // that never actually finished looking. That must fail the whole command, not print a zero
    // (finding 2).
    void GateStatusStalenessScanFailureIsNotVerified(const Fixture& fixture)
    {
        const auto brokenPath = fixture.Root / "NoFileTableDocuments.db";
        std::filesystem::copy_file(fixture.Documents, brokenPath, std::filesystem::copy_options::overwrite_existing);
        {
            auto db = Store::Database::Open(brokenPath, Store::Database::Mode::ReadWrite);
            Expect(db.has_value() && db->Execute("DROP TABLE File").has_value(), "the fixture drops the File table");
        }
        Fixture broken   = fixture;
        broken.Documents = brokenPath;

        std::vector<std::string> output;
        const auto                verdict = Cli::RunStatus(Environment(broken, output, true));
        Expect(verdict.Kind == Cli::VerdictKind::NotVerified,
               "status exits NOT VERIFIED when the staleness scan itself cannot run");
        Expect(std::none_of(output.begin(), output.end(),
                           [](const std::string& line) { return line.starts_with("changed:"); }),
               "status never prints a changed/added/removed line it could not actually establish");
    }

    // Builder.cpp excludes README.md/index.md by name and build/lab/.git by directory name from
    // the corpus it indexes; status's own "added" walk must apply the identical rule, or these
    // count as new forever even right after a clean build (finding 1). A private fixture: adding
    // these names to the shared one above would perturb its own exact section/citation counts.
    Fixture CreateExclusionFixture()
    {
        Fixture fixture;
        fixture.Root = std::filesystem::temp_directory_path() / "SherlockDocumentLayerExclusionGate";
        std::filesystem::remove_all(fixture.Root);
        std::filesystem::create_directories(fixture.Root / "docs" / "re");
        std::filesystem::create_directories(fixture.Root / "Source" / "build");
        std::filesystem::create_directories(fixture.Root / "Source" / "lab");
        std::filesystem::create_directories(fixture.Root / "Source" / ".git");
        std::filesystem::create_directories(fixture.Root / "Source" / "NewTower");
        fixture.Store = fixture.Root / "Store";
        std::filesystem::create_directories(fixture.Store);
        fixture.Documents = fixture.Root / "Documents.db";

        {
            auto catalog = Store::Database::Open(fixture.Store / "Catalog.db", Store::Database::Mode::ReadWrite);
            Expect(catalog.has_value(), "the exclusion fixture catalog opens");
            if (catalog)
            {
                Expect(Store::CreateCatalog(*catalog, "test-build", "test-cache-uuid").has_value(),
                       "the exclusion fixture uses the production schema API");
            }
        }

        const auto sample = fixture.Root / "docs" / "re" / "sample.md";
        { std::ofstream stream(sample, std::ios::binary); stream << "# Indexed\r\n"; }
        // These physically sit next to the corpus but a real build never indexes them -- and
        // status's walk must agree, or every one of them inflates "added" forever.
        { std::ofstream stream(fixture.Root / "docs" / "re" / "README.md", std::ios::binary); stream << "readme\n"; }
        { std::ofstream stream(fixture.Root / "docs" / "re" / "index.md", std::ios::binary); stream << "index\n"; }
        { std::ofstream stream(fixture.Root / "Source" / "build" / "Ignored.h", std::ios::binary); stream << "//\n"; }
        { std::ofstream stream(fixture.Root / "Source" / "lab" / "Ignored.h", std::ios::binary); stream << "//\n"; }
        { std::ofstream stream(fixture.Root / "Source" / ".git" / "Ignored.h", std::ios::binary); stream << "//\n"; }
        // The one file that is genuinely new: skipped by no rule, and not in the File table.
        { std::ofstream stream(fixture.Root / "Source" / "NewTower" / "New.h", std::ios::binary); stream << "//\n"; }

        {
            auto documents = Store::Database::Open(fixture.Documents, Store::Database::Mode::ReadWrite);
            Expect(documents.has_value(), "the exclusion fixture documents open");
            if (documents)
            {
                Expect(Store::CreateDocumentsStore(*documents).has_value() &&
                           Store::CreateDocumentIndexes(*documents).has_value(),
                       "the exclusion fixture uses the production schema APIs");
                auto insertFile = documents->Prepare("INSERT INTO File(Path, Size, MTime) VALUES(?1, ?2, ?3)");
                Expect(insertFile.has_value(), "the exclusion fixture File insert prepares");
                if (insertFile)
                {
                    Expect(insertFile->Bind(1, std::string_view("docs/re/sample.md")).has_value() &&
                               insertFile->Bind(2, static_cast<std::int64_t>(std::filesystem::file_size(sample))).has_value() &&
                               insertFile->Bind(3, MTime(sample)).has_value() && insertFile->Step().has_value(),
                           "the exclusion fixture records its one indexed file");
                }
            }
        }
        return fixture;
    }

    void GateStatusIgnoresBuilderExcludedFiles()
    {
        const auto fixture = CreateExclusionFixture();
        std::vector<std::string> output;
        const auto                verdict = Cli::RunStatus(Environment(fixture, output, true));
        Expect(verdict.Kind == Cli::VerdictKind::Found, "status answers over the exclusion fixture");
        Expect(std::find(output.begin(), output.end(),
                         "changed: 0, added: 1, removed: 0 (since this store was built)") != output.end(),
               "status counts only the genuinely new file -- README.md, index.md, and the "
               "build/lab/.git entries never inflate added");
        std::filesystem::remove_all(fixture.Root);
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
    GateCitedByAndSealedAtPropagateFailure(fixture);
    GateDocumentsUnreadableIsNotVerified(fixture);
    GateStatusStalenessScanFailureIsNotVerified(fixture);
    GateStatusIgnoresBuilderExcludedFiles();
    GateStatusDetectsChangedAndAdded(fixture);
    GateStatusDetectsRemoved(fixture);
    std::filesystem::remove_all(fixture.Root);
    return Finish();
}
