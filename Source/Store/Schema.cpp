// Sherlock — tools/Sherlock/Source/Store/Schema.cpp
// DDL of Catalog.db and Images/<Name>.db, and the check that refuses another version.
#include <Store/Schema.h>

#include <format>

namespace Sherlock::Store
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        constexpr std::string_view kCatalogTables = R"sql(
            CREATE TABLE IF NOT EXISTS Meta(Key TEXT PRIMARY KEY, Value TEXT NOT NULL) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS Image(Path TEXT PRIMARY KEY, Name TEXT NOT NULL, Tower TEXT,
                Header INTEGER NOT NULL, State TEXT NOT NULL, Reason TEXT, FactsVersion TEXT,
                HexRaysVersion TEXT) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS Coverage(Image TEXT NOT NULL, Layer TEXT NOT NULL, Unit TEXT NOT NULL,
                Read INTEGER NOT NULL, Total INTEGER NOT NULL, PRIMARY KEY(Image, Layer)) WITHOUT ROWID;
        )sql";

        constexpr std::string_view kImageTables = R"sql(
            CREATE TABLE IF NOT EXISTS Meta(Key TEXT PRIMARY KEY, Value TEXT NOT NULL) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS Name(Id INTEGER PRIMARY KEY, Text TEXT NOT NULL UNIQUE);
            CREATE TABLE IF NOT EXISTS Segment(Name TEXT NOT NULL, Address INTEGER PRIMARY KEY, Size INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS Section(Name TEXT PRIMARY KEY, Address INTEGER NOT NULL,
                Size INTEGER NOT NULL) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS Function(Address INTEGER PRIMARY KEY, Size INTEGER NOT NULL, Source TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS Symbol(Address INTEGER NOT NULL, Name INTEGER NOT NULL, Demangled INTEGER,
                External INTEGER NOT NULL, PRIMARY KEY(Address, Name)) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS Call(Site INTEGER PRIMARY KEY, Caller INTEGER NOT NULL,
                Target INTEGER NOT NULL, Island INTEGER, Via TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS LiteralRef(Site INTEGER NOT NULL, Target INTEGER NOT NULL, Caller INTEGER,
                Kind TEXT NOT NULL, Value REAL, PRIMARY KEY(Site, Target)) WITHOUT ROWID;
        )sql";

        constexpr std::string_view kImageIndexes = R"sql(
            CREATE INDEX IF NOT EXISTS CallTarget ON Call(Target);
            CREATE INDEX IF NOT EXISTS CallCaller ON Call(Caller);
            CREATE INDEX IF NOT EXISTS LiteralRefTarget ON LiteralRef(Target);
            CREATE INDEX IF NOT EXISTS SymbolName ON Symbol(Name);
        )sql";

        Expected<void> WriteMeta(Database& db, std::string_view key, std::string_view value)
        {
            auto statement = db.Prepare("INSERT OR REPLACE INTO Meta(Key, Value) VALUES(?1, ?2)");
            if (!statement)
            {
                return std::unexpected(statement.error());
            }
            if (auto ok = statement->Bind(1, key); !ok)
            {
                return ok;
            }
            if (auto ok = statement->Bind(2, value); !ok)
            {
                return ok;
            }
            if (auto step = statement->Step(); !step)
            {
                return std::unexpected(step.error());
            }
            return {};
        }

        Expected<void> CreateWithMeta(Database& db, std::string_view tables, std::string_view kind)
        {
            if (auto ok = db.Execute(tables); !ok)
            {
                return ok;
            }
            if (auto ok = WriteMeta(db, "Kind", kind); !ok)
            {
                return ok;
            }
            if (auto ok = WriteMeta(db, "SchemaVersion", std::to_string(kSchemaVersion)); !ok)
            {
                return ok;
            }
            return WriteMeta(db, "SherlockVersion", SHERLOCK_VERSION);
        }
    }

    Expected<void> CreateCatalog(Database& db, std::string_view build, std::string_view cacheUuid)
    {
        if (auto ok = CreateWithMeta(db, kCatalogTables, "Catalog"); !ok)
        {
            return ok;
        }
        if (auto ok = WriteMeta(db, "Build", build); !ok)
        {
            return ok;
        }
        return WriteMeta(db, "CacheUuid", cacheUuid);
    }

    Expected<void> CreateImageStore(Database& db, std::string_view imagePath)
    {
        if (auto ok = CreateWithMeta(db, kImageTables, "Image"); !ok)
        {
            return ok;
        }
        return WriteMeta(db, "ImagePath", imagePath);
    }

    Expected<void> CreateImageIndexes(Database& db)
    {
        return db.Execute(kImageIndexes);
    }

    Expected<void> CheckSchema(Database& db, std::string_view kind)
    {
        auto read = db.Prepare("SELECT (SELECT Value FROM Meta WHERE Key = 'Kind'), "
                               "(SELECT Value FROM Meta WHERE Key = 'SchemaVersion')");
        if (!read)
        {
            return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "CheckSchema", db.Path().string(),
                        "the file carries no Sherlock schema", "rebuild it with Sherlock build facts");
        }
        auto row = read->Step();
        const std::string actualKind(row && *row ? read->Text(0) : "");
        const std::string version(row && *row ? read->Text(1) : "");
        if (actualKind != kind || version != std::to_string(kSchemaVersion))
        {
            return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "CheckSchema", db.Path().string(),
                        std::format("found {} schema {}, expected {} schema {}", actualKind.empty() ? "no" : actualKind,
                                    version.empty() ? "none" : version, kind, kSchemaVersion),
                        "rebuild it with Sherlock build facts");
        }
        return {};
    }
}
