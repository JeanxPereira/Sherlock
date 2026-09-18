// Sherlock — tests/Sherlock/StoreGates.cpp
// Transaction rollback, statement reuse, schema refusal, and handle lifetime.
#include <Store/Database.h>
#include <Store/Schema.h>

#include "SherlockHarness.h"

#include <windows.h>

using namespace Sherlock;

namespace
{
    std::filesystem::path Fresh(const char* name)
    {
        const auto path = std::filesystem::temp_directory_path() / name;
        for (const char* suffix : {"", "-wal", "-shm"})
        {
            std::filesystem::remove(path.string() + suffix);
        }
        return path;
    }

    void GateRollbackAndReuse()
    {
        const auto path = Fresh("SherlockStoreGate.db");
        auto db = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "a new database opens read-write");
        Expect(Store::CreateImageStore(*db, "/usr/lib/test.dylib", "test-cache").has_value(), "the image schema is created");
        {
            auto tx = Store::Transaction::Begin(*db);
            Expect(db->Execute("INSERT INTO Function VALUES(1, 4, 'FunctionStarts')").has_value(), "insert inside a transaction");
        }
        ExpectEq(db->ScalarInt("SELECT count(*) FROM Function").value(), std::int64_t{0},
                 "a transaction destroyed without Commit rolls back");

        auto tx = Store::Transaction::Begin(*db);
        auto insert = db->Prepare("INSERT INTO Function VALUES(?1, ?2, 'FunctionStarts')");
        Expect(insert.has_value(), "the insert prepares once");
        for (std::int64_t i = 0; i < 1000; ++i)
        {
            Expect(insert->Bind(1, 0x240000000 + i * 4).has_value() && insert->Bind(2, std::int64_t{4}).has_value(), "bind");
            Expect(insert->Step().has_value() && insert->Reset().has_value(), "step and reset");
        }
        Expect(tx->Commit().has_value(), "commit");
        ExpectEq(db->ScalarInt("SELECT count(*) FROM Function").value(), std::int64_t{1000},
                 "one prepared statement inserts 1000 rows");

        const auto bad = db->Execute("SELECT * FROM NoSuchTable");
        Expect(!bad.has_value() && bad.error().Code == Foundation::DiagnosticCode::Database &&
                   bad.error().System.find("no such table") != std::string::npos,
               "a SQL error carries SQLite's own message");
    }

    void GateSchemaRefusal()
    {
        const auto path = Fresh("SherlockSchemaGate.db");
        auto db = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(Store::CreateCatalog(*db, "26A5416b", "f06856bf36f8349c892d74941baa031b").has_value(), "catalog created");
        Expect(Store::CheckSchema(*db, "Catalog").has_value(), "a fresh catalog passes its own check");
        Expect(!Store::CheckSchema(*db, "Image").has_value(), "a catalog is refused where an image store is expected");
        Expect(db->Execute("UPDATE Meta SET Value = '0' WHERE Key = 'SchemaVersion'").has_value(), "downgrade");
        const auto old = Store::CheckSchema(*db, "Catalog");
        Expect(!old.has_value() && old.error().Code == Foundation::DiagnosticCode::Mismatch &&
                   old.error().Level == Foundation::Severity::NotVerified,
               "another schema version is NOT VERIFIED, not a pass");
    }

    void GateDocumentsSchemaIndependentOfCatalog()
    {
        const auto catalogPath = Fresh("SherlockCatalogSchemaGate.db");
        const auto documentsPath = Fresh("SherlockDocumentsSchemaGate.db");
        auto catalog = Store::Database::Open(catalogPath, Store::Database::Mode::ReadWrite);
        auto documents = Store::Database::Open(documentsPath, Store::Database::Mode::ReadWrite);
        Expect(catalog.has_value() && documents.has_value(), "catalog and documents databases open");
        if (!catalog || !documents) return;

        Expect(Store::CreateCatalog(*catalog, "26A5416b", "f06856bf36f8349c892d74941baa031b").has_value(),
               "catalog schema is created");
        Expect(Store::CreateDocumentsStore(*documents).has_value(), "documents schema is created");
        Expect(Store::CreateDocumentIndexes(*documents).has_value(), "documents indexes are created");
        Expect(Store::CheckSchema(*catalog, "Catalog").has_value(), "catalog uses its own schema version");
        Expect(Store::CheckDocumentsSchema(*documents).has_value(), "documents uses its own schema version");
        Expect(!Store::CheckDocumentsSchema(*catalog).has_value(),
               "a catalog is refused where a documents store is expected");

        const auto kind = Store::ReadMeta(*documents, "Kind");
        const auto version = Store::ReadMeta(*documents, "SchemaVersion");
        Expect(kind.has_value() && *kind == "Documents", "documents metadata records Kind");
        Expect(version.has_value() && *version == std::to_string(Store::kDocumentsSchemaVersion),
               "documents metadata records its independent schema version");

        for (const std::string_view table : {"Meta", "Coverage", "File", "Section", "SectionFtsTitle", "SectionFtsText",
                                             "Citation", "Seal"})
        {
            auto statement = documents->Prepare("SELECT count(*) FROM sqlite_master WHERE name = ?1");
            Expect(statement.has_value() && statement->Bind(1, table).has_value() && statement->Step().value() &&
                       statement->Int(0) == 1,
                   "every required documents table exists");
        }
        for (const std::string_view index : {"CitationAddress", "CitationSymbol", "SealAddress"})
        {
            auto statement = documents->Prepare("SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name = ?1");
            Expect(statement.has_value() && statement->Bind(1, index).has_value() && statement->Step().value() &&
                       statement->Int(0) == 1,
                   "every required documents index exists");
        }

        Expect(documents->Execute("UPDATE Meta SET Value = '0' WHERE Key = 'SchemaVersion'").has_value(),
               "corrupt documents schema version");
        const auto corrupt = Store::CheckDocumentsSchema(*documents);
        Expect(!corrupt.has_value() && corrupt.error().Code == Foundation::DiagnosticCode::Mismatch &&
                   corrupt.error().Level == Foundation::Severity::NotVerified,
               "corrupt documents metadata is NOT VERIFIED, not a pass");
    }

    void GateHandleLifetime()
    {
        const auto path = Fresh("SherlockHandleGate.db");
        {
            auto db = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
            Expect(Store::CreateImageStore(*db, "/usr/lib/test.dylib", "test-cache").has_value(), "schema");
        }
        DWORD before = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &before);
        for (int i = 0; i < 200; ++i)
        {
            auto db = Store::Database::Open(path, Store::Database::Mode::ReadOnly);
            auto count = db->Prepare("SELECT count(*) FROM Function");
            Expect(count.has_value() && count->Step().value(), "read-only query runs");
        }
        DWORD after = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &after);
        ExpectEq(after, before, "200 open/query/close cycles leak no handle");
    }

    void GateSqliteFullSeverity()
    {
        const auto path = Fresh("SherlockFullGate.db");
        auto db = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "disk-full gate database opens");
        if (!db) return;
        Expect(db->Execute("PRAGMA page_size=512; PRAGMA max_page_count=2; CREATE TABLE T(Value BLOB);").has_value(),
               "disk-full fixture is configured");
        const auto result = db->Execute("INSERT INTO T VALUES(zeroblob(4096))");
        Expect(!result.has_value() && result.error().Code == Foundation::DiagnosticCode::Io &&
                   result.error().Level == Foundation::Severity::NotVerified,
               "SQLITE_FULL is a NOT VERIFIED resource failure");
    }
}

int main()
{
    try
    {
        GateRollbackAndReuse();
        GateSchemaRefusal();
        GateDocumentsSchemaIndependentOfCatalog();
        GateHandleLifetime();
        GateSqliteFullSeverity();
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: an exception escaped a gate: %s\n", e.what());
        return 1;
    }
    return Finish();
}
