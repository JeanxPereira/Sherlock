// Sherlock — tests/Sherlock/DocumentsGates.cpp
// Direct Documents.db fixture gates for find and laudo.
#include <SherlockCli/Documents.h>
#include <SherlockCli/Queries.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include "SherlockHarness.h"

#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace Sherlock;

namespace
{
    struct Fixture
    {
        std::filesystem::path Root;
        std::filesystem::path Documents;
        std::string           LaudoText;
        std::string           ConceptText;
    };

    std::int64_t MTime(const std::filesystem::path& path)
    {
        return std::filesystem::last_write_time(path).time_since_epoch().count();
    }

    void InsertFile(Store::Database& db, std::string_view path, const std::filesystem::path& disk)
    {
        auto statement = db.Prepare("INSERT INTO File(Path, Size, MTime) VALUES(?1, ?2, ?3)");
        Expect(statement.has_value(), "the fixture File insert prepares");
        if (!statement) return;
        Expect(statement->Bind(1, path).has_value() &&
                   statement->Bind(2, static_cast<std::int64_t>(std::filesystem::file_size(disk))).has_value() &&
                   statement->Bind(3, MTime(disk)).has_value() && statement->Step().has_value(),
               "the fixture records its backing file stamp");
    }

    void InsertSection(Store::Database& db, std::string_view file, std::string_view number, std::string_view title,
                       std::string_view text)
    {
        auto section = db.Prepare("INSERT INTO Section(File, Number, Title, FirstLine, LastLine, Text) "
                                  "VALUES(?1, ?2, ?3, 1, 3, ?4)");
        Expect(section.has_value(), "the fixture Section insert prepares");
        if (!section) return;
        Expect(section->Bind(1, file).has_value() && section->Bind(2, number).has_value() &&
                   section->Bind(3, title).has_value() && section->Bind(4, text).has_value() && section->Step().has_value(),
               "the fixture inserts one document section");
        auto fts = db.Prepare("INSERT INTO SectionFtsText(rowid, Text) VALUES(?1, ?2)");
        Expect(fts.has_value(), "the fixture text FTS insert prepares");
        if (!fts) return;
        Expect(fts->Bind(1, db.LastInsertId()).has_value() && fts->Bind(2, text).has_value() && fts->Step().has_value(),
               "the fixture indexes one document section");
    }

    Fixture CreateFixture()
    {
        Fixture fixture;
        fixture.Root = std::filesystem::temp_directory_path() / "SherlockDocumentsQueriesGate";
        std::filesystem::remove_all(fixture.Root);
        std::filesystem::create_directories(fixture.Root / "docs" / "re");
        std::filesystem::create_directories(fixture.Root / "docs" / "concepts");
        fixture.Documents = fixture.Root / "Documents.db";
        fixture.LaudoText = "## §7 LayerResolver\r\n\r\n100% shadow pool.\r\n";
        fixture.ConceptText = "A concept page\n";
        const auto laudo = fixture.Root / "docs" / "re" / "sample.md";
        const auto concept = fixture.Root / "docs" / "concepts" / "0x27c198c20.md";
        {
            std::ofstream stream(laudo, std::ios::binary);
            stream << fixture.LaudoText;
        }
        {
            std::ofstream stream(concept, std::ios::binary);
            stream << fixture.ConceptText;
        }

        auto db = Store::Database::Open(fixture.Documents, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "the documents fixture opens");
        if (!db) return fixture;
        Expect(Store::CreateDocumentsStore(*db).has_value() && Store::CreateDocumentIndexes(*db).has_value(),
               "the documents fixture uses the production schema APIs");
        Expect(db->Execute("INSERT INTO Coverage(Root, Read, Total) VALUES('docs/re', 2, 2)").has_value(),
               "the documents fixture writes coverage");
        InsertFile(*db, "docs/re/sample.md", laudo);
        InsertFile(*db, "docs/concepts/0x27c198c20.md", concept);
        InsertSection(*db, "docs/re/sample.md", "7", "LayerResolver", fixture.LaudoText);
        InsertSection(*db, "docs/concepts/0x27c198c20.md", "", "Concept Title", fixture.ConceptText);
        return fixture;
    }

    Cli::QueryEnvironment Environment(const Fixture& fixture, std::vector<std::string>& output, bool json = true)
    {
        return {{}, fixture.Documents, fixture.Root, false, json, &output};
    }

    void GateLaudoByteExactAndJsonParity(const Fixture& fixture)
    {
        std::vector<std::string> jsonOutput;
        const auto json = Cli::RunLaudo(Environment(fixture, jsonOutput), "sample", "§7");
        Expect(json.Kind == Cli::VerdictKind::Found && jsonOutput.size() == 1 && jsonOutput.front() == fixture.LaudoText,
               "laudo stores the exact CRLF section text for JSON output");

        std::vector<std::string> plainOutput;
        const auto plain = Cli::RunLaudo(Environment(fixture, plainOutput, false), "sample", "7");
        Expect(plain.Kind == Cli::VerdictKind::Found && plainOutput == jsonOutput,
               "plain laudo emits only the same bytes stored for JSON output");

        std::vector<std::string> lfOutput;
        const auto lf = Cli::RunLaudo(Environment(fixture, lfOutput), "0x27c198c20", "concept title");
        Expect(lf.Kind == Cli::VerdictKind::Found && lfOutput == std::vector<std::string>{fixture.ConceptText},
               "laudo preserves stored LF text as well as CRLF text");
    }

    void GateCaseInsensitiveTitleAndNoFactsStore(const Fixture& fixture)
    {
        std::vector<std::string> output;
        const auto laudo = Cli::RunLaudo(Environment(fixture, output), "sample", "layerresolver");
        Expect(laudo.Kind == Cli::VerdictKind::Found && output == std::vector<std::string>{fixture.LaudoText},
               "laudo falls back to a case-insensitive title match");
        output.clear();
        const auto find = Cli::RunFind(Environment(fixture, output), "shadow pool");
        Expect(find.Kind == Cli::VerdictKind::Found && !output.empty() && output.front().find("sample.md 7 -- LayerResolver") != std::string::npos,
               "find works with Documents.db and no facts store");
    }

    void GateStalenessAndSqliteFailures(const Fixture& fixture)
    {
        const auto laudo = fixture.Root / "docs" / "re" / "sample.md";
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::ofstream stream(laudo, std::ios::binary | std::ios::app);
            stream << "changed\r\n";
        }
        std::vector<std::string> output;
        const auto staleLaudo = Cli::RunLaudo(Environment(fixture, output), "sample", "7");
        Expect(staleLaudo.Kind == Cli::VerdictKind::NotVerified && output.empty(),
               "changed laudo exits NOT VERIFIED without serving stale text");
        const auto staleFind = Cli::RunFind(Environment(fixture, output), "shadow pool");
        Expect(staleFind.Kind == Cli::VerdictKind::Found && !output.empty() && output.front().find("[stale]") != std::string::npos,
               "find marks a stale hit without hiding it");

        auto db = Store::Database::Open(fixture.Documents, Store::Database::Mode::ReadWrite);
        Expect(db.has_value() && db->Execute("DROP TABLE Section").has_value(), "the fixture creates a query-time SQLite failure");
        output.clear();
        const auto failure = Cli::RunFind(Environment(fixture, output), "shadow pool");
        Expect(failure.Kind == Cli::VerdictKind::NotVerified && !failure.Why.empty(),
               "SQLite prepare failures propagate instead of becoming empty results");
    }
}

int main()
{
    const auto fixture = CreateFixture();
    GateLaudoByteExactAndJsonParity(fixture);
    GateCaseInsensitiveTitleAndNoFactsStore(fixture);
    GateStalenessAndSqliteFailures(fixture);
    std::filesystem::remove_all(fixture.Root);
    return Finish();
}
