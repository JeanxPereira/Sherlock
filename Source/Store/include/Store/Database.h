// Sherlock — tools/Sherlock/Source/Store/include/Store/Database.h
// Owning wrappers for an SQLite connection, a prepared statement and a transaction (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace Sherlock::Store
{
    using Foundation::Expected;

    class Statement
    {
    public:
        Statement(Statement&& other) noexcept;
        Statement& operator=(Statement&& other) noexcept;
        Statement(const Statement&)            = delete;
        Statement& operator=(const Statement&) = delete;
        ~Statement();

        Expected<void> Bind(int index, std::int64_t value);
        Expected<void> Bind(int index, double value);
        Expected<void> Bind(int index, std::string_view value);
        Expected<void> BindNull(int index);

        // True when a row is ready to read; false when the statement is done.
        Expected<bool> Step();

        // Back to the first row, with every binding cleared.
        Expected<void> Reset();

        std::int64_t     Int(int column) const;
        double           Real(int column) const;
        std::string_view Text(int column) const;
        bool             IsNull(int column) const;

    private:
        friend class Database;
        Statement(sqlite3* db, sqlite3_stmt* statement) noexcept : _db(db), _statement(statement) {}
        Expected<void> Check(int rc, const char* operation) const;

        sqlite3*      _db        = nullptr;
        sqlite3_stmt* _statement = nullptr;
    };

    class Database
    {
    public:
        enum class Mode
        {
            ReadOnly,
            ReadWrite,
        };

        static Expected<Database> Open(const std::filesystem::path& path, Mode mode);

        Database(Database&& other) noexcept;
        Database& operator=(Database&& other) noexcept;
        Database(const Database&)            = delete;
        Database& operator=(const Database&) = delete;
        ~Database();

        Expected<void>         Execute(std::string_view sql);
        Expected<Statement>    Prepare(std::string_view sql);
        Expected<void>         Attach(const std::filesystem::path& path, std::string_view alias);
        Expected<std::int64_t> ScalarInt(std::string_view sql);
        std::int64_t           LastInsertId() const;

        const std::filesystem::path& Path() const noexcept { return _path; }

    private:
        Database() = default;
        std::unexpected<Foundation::Diagnostic> Error(const char* operation, std::string_view subject) const;

        sqlite3*              _db = nullptr;
        std::filesystem::path _path;
    };

    class Transaction
    {
    public:
        static Expected<Transaction> Begin(Database& db);

        Transaction(Transaction&& other) noexcept;
        Transaction& operator=(Transaction&&)      = delete;
        Transaction(const Transaction&)            = delete;
        Transaction& operator=(const Transaction&) = delete;
        ~Transaction();

        Expected<void> Commit();

    private:
        explicit Transaction(Database& db) noexcept : _db(&db) {}

        Database* _db = nullptr;
    };
}
