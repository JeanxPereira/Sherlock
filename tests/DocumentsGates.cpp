// Sherlock — tests/Sherlock/DocumentsGates.cpp
// Direct Documents.db fixture gates for find and laudo.
#include <DocumentIndex/Builder.h>
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
        const auto conceptFile = fixture.Root / "docs" / "concepts" / "0x27c198c20.md";
        {
            std::ofstream stream(laudo, std::ios::binary);
            stream << fixture.LaudoText;
        }
        {
            std::ofstream stream(conceptFile, std::ios::binary);
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
        InsertFile(*db, "docs/concepts/0x27c198c20.md", conceptFile);
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
        const auto sectionTitle = Cli::RunLaudo(Environment(fixture, output), "sample", "§layerresolver");
        Expect(sectionTitle.Kind == Cli::VerdictKind::Found && output == std::vector<std::string>{fixture.LaudoText},
               "laudo normalizes an optional section marker before title fallback");
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

    void GateDocumentsSchemaRefusal()
    {
        const auto fixture = CreateFixture();
        {
            auto db = Store::Database::Open(fixture.Documents, Store::Database::Mode::ReadWrite);
            Expect(db.has_value() && db->Execute("UPDATE Meta SET Value = '0' WHERE Key = 'SchemaVersion'").has_value(),
                   "the fixture corrupts the documents schema metadata");
        }
        std::vector<std::string> output;
        const auto find = Cli::RunFind(Environment(fixture, output), "shadow pool");
        Expect(find.Kind == Cli::VerdictKind::NotVerified && find.Why.find("CheckSchema") != std::string::npos,
               "find refuses a Documents.db with the wrong schema version");
        const auto laudo = Cli::RunLaudo(Environment(fixture, output), "sample", "7");
        Expect(laudo.Kind == Cli::VerdictKind::NotVerified && laudo.Why.find("CheckSchema") != std::string::npos,
               "laudo refuses a Documents.db with the wrong schema version");
        std::filesystem::remove_all(fixture.Root);
    }

    void GatePlainLaudoOutputFailure(const Fixture& fixture)
    {
        std::vector<std::string> output;
        auto env = Environment(fixture, output, false);
        env.PlainTextSink = [](std::string_view) {
            return Foundation::Fail(Foundation::DiagnosticCode::Io, Foundation::Severity::NotVerified,
                                    "PlainTextSink", "stdout", "injected output failure", "retry the command");
        };
        const auto verdict = Cli::RunLaudo(env, "sample", "7");
        Expect(verdict.Kind == Cli::VerdictKind::NotVerified && output.empty(),
               "laudo reports a plain output failure without returning a successful payload");
    }

    // A string under 3 significant characters cannot produce a single trigram, so
    // SectionFtsText's MATCH is structurally unable to answer it -- find must refuse it as NOT
    // VERIFIED, not report a silent, always-empty "not in the corpus" (finding 6). Runs before
    // GateStalenessAndSqliteFailures drops Section, so its own "shadow pool" positive control
    // still has a table to query.
    void GateSubTrigramQueryIsNotVerified(const Fixture& fixture)
    {
        std::vector<std::string> output;
        const auto twoLetters = Cli::RunFind(Environment(fixture, output), "UI");
        Expect(twoLetters.Kind == Cli::VerdictKind::NotVerified && output.empty(),
               "find refuses a 2-character query instead of silently reporting EMPTY");
        output.clear();
        const auto hexPrefix = Cli::RunFind(Environment(fixture, output), "0x");
        Expect(hexPrefix.Kind == Cli::VerdictKind::NotVerified && output.empty(),
               "find refuses a 2-character hex-prefix query the trigram index cannot answer");
        output.clear();
        const auto real = Cli::RunFind(Environment(fixture, output), "shadow pool");
        Expect(real.Kind == Cli::VerdictKind::Found, "find still answers a query at or above the trigram floor");
    }

    // laudo matches a slug by path suffix across every indexed root (docs/re and docs/concepts);
    // a basename present under both would otherwise splice two documents into one outline and
    // stat only the first for staleness. A private fixture, since deliberately colliding with
    // the shared fixture's own "sample" slug would break every other gate above (finding 8).
    void GateAmbiguousSlugIsNotVerified()
    {
        Fixture fixture;
        fixture.Root = std::filesystem::temp_directory_path() / "SherlockAmbiguousSlugGate";
        std::filesystem::remove_all(fixture.Root);
        std::filesystem::create_directories(fixture.Root / "docs" / "re");
        std::filesystem::create_directories(fixture.Root / "docs" / "concepts");
        fixture.Documents = fixture.Root / "Documents.db";

        // Scoped: the connection must close before RunLaudo (re)opens the same file -- and
        // before remove_all below, which a lingering handle would make fail on Windows.
        {
            auto db = Store::Database::Open(fixture.Documents, Store::Database::Mode::ReadWrite);
            Expect(db.has_value(), "the ambiguous-slug fixture opens");
            if (db)
            {
                Expect(Store::CreateDocumentsStore(*db).has_value() && Store::CreateDocumentIndexes(*db).has_value(),
                       "the ambiguous-slug fixture uses the production schema APIs");
                InsertSection(*db, "docs/re/twin.md", "", "Twin In Laudos", "laudo text\n");
                InsertSection(*db, "docs/concepts/twin.md", "", "Twin In Concepts", "concept text\n");
            }
        }

        std::vector<std::string> output;
        const auto laudo = Cli::RunLaudo(Environment(fixture, output), "twin", "");
        Expect(laudo.Kind == Cli::VerdictKind::NotVerified && laudo.Why.find("more than one file") != std::string::npos,
               "laudo refuses an ambiguous slug instead of silently picking one of the two colliding files");
        std::filesystem::remove_all(fixture.Root);
    }

    // Every Section a real build writes has a matching File row for its own source file
    // (Builder.cpp always writes File before Section, for every indexed document); a Section
    // with none is Documents.db missing its own bookkeeping, not a fresh file. Reporting "false"
    // (fresh) for this is a silent "could not look" feeding laudo's byte-exact promise (finding 4).
    void GateMissingFileRowIsNotVerified()
    {
        Fixture fixture;
        fixture.Root = std::filesystem::temp_directory_path() / "SherlockOrphanSectionGate";
        std::filesystem::remove_all(fixture.Root);
        std::filesystem::create_directories(fixture.Root / "docs" / "re");
        fixture.Documents = fixture.Root / "Documents.db";

        {
            auto db = Store::Database::Open(fixture.Documents, Store::Database::Mode::ReadWrite);
            Expect(db.has_value(), "the orphan-section fixture opens");
            if (db)
            {
                Expect(Store::CreateDocumentsStore(*db).has_value() && Store::CreateDocumentIndexes(*db).has_value(),
                       "the orphan-section fixture uses the production schema APIs");
                InsertSection(*db, "docs/re/orphan.md", "", "Orphan", "orphan text\n");
            }
        }

        std::vector<std::string> output;
        const auto laudo = Cli::RunLaudo(Environment(fixture, output), "orphan", "");
        Expect(laudo.Kind == Cli::VerdictKind::NotVerified && output.empty() &&
                   laudo.Why.find("File row is missing") != std::string::npos,
               "laudo refuses to serve a section whose own File row is missing, instead of treating it as fresh");
        std::filesystem::remove_all(fixture.Root);
    }

    // Writing the new Documents.db directly over a prior good one means any failure past that
    // point (a bad extractor read, a disk-full mid-transaction) leaves a table-less file where a
    // good store sat a moment ago, and q/status would report "not built" about a file that is
    // present and working. BuildDocuments instead builds into a sibling temp path and publishes
    // it only after a successful commit (finding 7).
    void GateBuildDocumentsPublishesAtomically()
    {
        const auto root = std::filesystem::temp_directory_path() / "SherlockBuildDocumentsAtomicGate";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "docs" / "re");
        std::filesystem::create_directories(root / "docs" / "concepts");
        std::filesystem::create_directories(root / "Source");
        // A minimal detached-HEAD git directory -- ReadCurrentHead needs .git/HEAD to resolve,
        // and a plain 40-hex line (no "ref:" prefix) skips ref resolution entirely.
        std::filesystem::create_directories(root / ".git");
        {
            std::ofstream stream(root / ".git" / "HEAD", std::ios::binary);
            stream << "0000000000000000000000000000000000000000\n";
        }
        {
            std::ofstream stream(root / "docs" / "re" / "good.md", std::ios::binary);
            stream << "## \xC2\xA7"
                      "1 Good\r\n\r\ngood text\r\n";
        }
        const auto documentsPath = root / "Documents.db";

        const auto first = DocumentIndex::BuildDocuments(root, documentsPath);
        Expect(first.has_value(), "the first build succeeds and produces a good store");
        if (!first)
        {
            std::filesystem::remove_all(root);
            return;
        }
        const auto goodSize = std::filesystem::file_size(documentsPath);
        Expect(goodSize > 0, "the first build's Documents.db is non-empty");

        // Replace docs/re with a regular file: WalkMarkdown's directory_iterator over it fails,
        // forcing the second build to return an error partway through -- after the good store
        // above already exists at documentsPath.
        std::filesystem::remove_all(root / "docs" / "re");
        {
            std::ofstream stream(root / "docs" / "re", std::ios::binary);
            stream << "not a directory";
        }

        const auto second = DocumentIndex::BuildDocuments(root, documentsPath);
        Expect(!second.has_value(),
               "a build that cannot even walk docs/re fails -- a sanity check on this gate's own setup");

        Expect(std::filesystem::exists(documentsPath), "the prior good Documents.db still exists after a failed rebuild");
        std::error_code sizeError;
        Expect(std::filesystem::file_size(documentsPath, sizeError) == goodSize && !sizeError,
               "a failed rebuild never replaces the prior good Documents.db with a partial one");
        Expect(!std::filesystem::exists(std::filesystem::path(documentsPath.string() + ".tmp")),
               "a failed rebuild leaves no stray .tmp file behind");

        {
            auto reopened = Store::Database::Open(documentsPath, Store::Database::Mode::ReadOnly);
            Expect(reopened.has_value() && Store::CheckDocumentsSchema(*reopened).has_value(),
                   "the surviving Documents.db still has a valid, checkable schema");
        }

        std::filesystem::remove_all(root);
    }
}

int main()
{
    const auto fixture = CreateFixture();
    GateLaudoByteExactAndJsonParity(fixture);
    GateCaseInsensitiveTitleAndNoFactsStore(fixture);
    GatePlainLaudoOutputFailure(fixture);
    GateSubTrigramQueryIsNotVerified(fixture);
    GateStalenessAndSqliteFailures(fixture);
    GateDocumentsSchemaRefusal();
    GateAmbiguousSlugIsNotVerified();
    GateMissingFileRowIsNotVerified();
    GateBuildDocumentsPublishesAtomically();
    std::filesystem::remove_all(fixture.Root);
    return Finish();
}
