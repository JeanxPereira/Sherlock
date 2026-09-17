// Sherlock — tools/Sherlock/Source/Store/Database.cpp
// SQLite calls behind Database, Statement and Transaction.
#include <Store/Database.h>

#include <sqlite3.h>

#include <format>
#include <utility>

namespace Sherlock::Store
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        // WAL keeps a store readable while a build writes it; the map size is the read window, 256 MiB.
        constexpr std::string_view kReadWritePragmas =
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY; "
            "PRAGMA mmap_size=268435456; PRAGMA foreign_keys=OFF;";
        constexpr std::string_view kReadOnlyPragmas = "PRAGMA temp_store=MEMORY; PRAGMA mmap_size=268435456;";
    }

    Expected<void> Statement::Check(int rc, const char* operation) const
    {
        if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE)
        {
            return {};
        }
        return Fail(DiagnosticCode::Database, Severity::Failed, operation, ::sqlite3_sql(_statement),
                    "SQLite refused the statement", "read the SQLite message", ::sqlite3_errmsg(_db));
    }

    Statement::Statement(Statement&& other) noexcept
        : _db(std::exchange(other._db, nullptr)), _statement(std::exchange(other._statement, nullptr))
    {
    }

    Statement& Statement::operator=(Statement&& other) noexcept
    {
        if (this != &other)
        {
            ::sqlite3_finalize(_statement);
            _db        = std::exchange(other._db, nullptr);
            _statement = std::exchange(other._statement, nullptr);
        }
        return *this;
    }

    Statement::~Statement()
    {
        ::sqlite3_finalize(_statement);
    }

    Expected<void> Statement::Bind(int index, std::int64_t value)
    {
        return Check(::sqlite3_bind_int64(_statement, index, value), "Statement::Bind");
    }

    Expected<void> Statement::Bind(int index, double value)
    {
        return Check(::sqlite3_bind_double(_statement, index, value), "Statement::Bind");
    }

    Expected<void> Statement::Bind(int index, std::string_view value)
    {
        return Check(::sqlite3_bind_text(_statement, index, value.data(), static_cast<int>(value.size()),
                                         SQLITE_TRANSIENT),
                     "Statement::Bind");
    }

    Expected<void> Statement::BindNull(int index)
    {
        return Check(::sqlite3_bind_null(_statement, index), "Statement::BindNull");
    }

    Expected<bool> Statement::Step()
    {
        const int rc = ::sqlite3_step(_statement);
        if (auto ok = Check(rc, "Statement::Step"); !ok)
        {
            return std::unexpected(ok.error());
        }
        return rc == SQLITE_ROW;
    }

    Expected<void> Statement::Reset()
    {
        ::sqlite3_clear_bindings(_statement);
        return Check(::sqlite3_reset(_statement), "Statement::Reset");
    }

    std::int64_t Statement::Int(int column) const
    {
        return ::sqlite3_column_int64(_statement, column);
    }

    double Statement::Real(int column) const
    {
        return ::sqlite3_column_double(_statement, column);
    }

    std::string_view Statement::Text(int column) const
    {
        const auto* text = ::sqlite3_column_text(_statement, column);
        return text == nullptr ? std::string_view{}
                               : std::string_view(reinterpret_cast<const char*>(text),
                                                  static_cast<std::size_t>(::sqlite3_column_bytes(_statement, column)));
    }

    bool Statement::IsNull(int column) const
    {
        return ::sqlite3_column_type(_statement, column) == SQLITE_NULL;
    }

    Expected<Database> Database::Open(const std::filesystem::path& path, Mode mode)
    {
        Database db;
        db._path        = path;
        const int flags = mode == Mode::ReadOnly ? SQLITE_OPEN_READONLY
                                                 : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        const std::string utf8 = path.string();
        if (::sqlite3_open_v2(utf8.c_str(), &db._db, flags | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
        {
            const std::string message = db._db ? ::sqlite3_errmsg(db._db) : "out of memory";
            return Fail(DiagnosticCode::Database, Severity::NotVerified, "Database::Open", utf8,
                        "the database cannot be opened",
                        mode == Mode::ReadOnly ? "build the store first: Sherlock build facts" : "check the directory",
                        message);
        }
        ::sqlite3_busy_timeout(db._db, 5000);
        if (auto ok = db.Execute(mode == Mode::ReadOnly ? kReadOnlyPragmas : kReadWritePragmas); !ok)
        {
            return std::unexpected(ok.error());
        }
        return db;
    }

    Database::Database(Database&& other) noexcept
        : _db(std::exchange(other._db, nullptr)), _path(std::move(other._path))
    {
    }

    Database& Database::operator=(Database&& other) noexcept
    {
        if (this != &other)
        {
            ::sqlite3_close_v2(_db);
            _db   = std::exchange(other._db, nullptr);
            _path = std::move(other._path);
        }
        return *this;
    }

    Database::~Database()
    {
        ::sqlite3_close_v2(_db);
    }

    std::unexpected<Foundation::Diagnostic> Database::Error(const char* operation, std::string_view subject) const
    {
        return Fail(DiagnosticCode::Database, Severity::Failed, operation, std::string(subject),
                    "SQLite refused the statement", "read the SQLite message", ::sqlite3_errmsg(_db));
    }

    Expected<void> Database::Execute(std::string_view sql)
    {
        const std::string text(sql);
        if (::sqlite3_exec(_db, text.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        {
            return Error("Database::Execute", sql);
        }
        return {};
    }

    Expected<Statement> Database::Prepare(std::string_view sql)
    {
        sqlite3_stmt* statement = nullptr;
        if (::sqlite3_prepare_v3(_db, sql.data(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT,
                                 &statement, nullptr) != SQLITE_OK)
        {
            return Error("Database::Prepare", sql);
        }
        return Statement(_db, statement);
    }

    Expected<void> Database::Attach(const std::filesystem::path& path, std::string_view alias)
    {
        auto attach = Prepare(std::format("ATTACH DATABASE ?1 AS \"{}\"", alias));
        if (!attach)
        {
            return std::unexpected(attach.error());
        }
        if (auto ok = attach->Bind(1, std::string_view(path.string())); !ok)
        {
            return ok;
        }
        if (auto step = attach->Step(); !step)
        {
            return std::unexpected(step.error());
        }
        return {};
    }

    Expected<std::int64_t> Database::ScalarInt(std::string_view sql)
    {
        auto statement = Prepare(sql);
        if (!statement)
        {
            return std::unexpected(statement.error());
        }
        auto row = statement->Step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            return Fail(DiagnosticCode::NotFound, Severity::Failed, "Database::ScalarInt", std::string(sql),
                        "the query returned no row", "check the query");
        }
        return statement->Int(0);
    }

    std::int64_t Database::LastInsertId() const
    {
        return ::sqlite3_last_insert_rowid(_db);
    }

    Expected<Transaction> Transaction::Begin(Database& db)
    {
        if (auto ok = db.Execute("BEGIN IMMEDIATE"); !ok)
        {
            return std::unexpected(ok.error());
        }
        return Transaction(db);
    }

    Transaction::Transaction(Transaction&& other) noexcept : _db(std::exchange(other._db, nullptr))
    {
    }

    Transaction::~Transaction()
    {
        if (_db != nullptr)
        {
            (void)_db->Execute("ROLLBACK");
        }
    }

    Expected<void> Transaction::Commit()
    {
        auto ok = _db->Execute("COMMIT");
        if (ok)
        {
            _db = nullptr;
        }
        return ok;
    }
}
