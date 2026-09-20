// Sherlock — tools/Sherlock/Source/HexRaysExport/Store.cpp
// Creating, writing and reading Images/<Image>.HexRays.db, pseudocode compressed on the way in (derived).

#include <HexRaysExport/Store.h>

#include <Store/Schema.h>

#include <zstd.h>

#include <format>

namespace Sherlock::HexRaysExport
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        constexpr std::string_view kKind = "HexRays";

        // Level 3 is zstd's default: the fill is bound by Hex-Rays, not by the compressor, and a
        // higher level buys single-digit percent for several times the CPU.
        constexpr int kCompressionLevel = 3;

        constexpr std::string_view kTables = R"sql(
            CREATE TABLE IF NOT EXISTS Meta(Key TEXT PRIMARY KEY, Value TEXT NOT NULL) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS Decompilation(Function INTEGER PRIMARY KEY, Pseudocode BLOB,
                Plain INTEGER NOT NULL, Lines INTEGER NOT NULL, Status TEXT NOT NULL, Reason TEXT,
                Seconds REAL NOT NULL) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS IdaName(Address INTEGER PRIMARY KEY, Name TEXT NOT NULL) WITHOUT ROWID;
        )sql";

        Expected<void> WriteMeta(Store::Database& db, std::string_view key, std::string_view value)
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

        Expected<std::optional<std::string>> MetaValue(Store::Database& db, std::string_view key)
        {
            auto statement = db.Prepare("SELECT Value FROM Meta WHERE Key = ?1");
            if (!statement)
            {
                return std::unexpected(statement.error());
            }
            if (auto ok = statement->Bind(1, key); !ok)
            {
                return std::unexpected(ok.error());
            }
            auto row = statement->Step();
            if (!row)
            {
                return std::unexpected(row.error());
            }
            if (!*row)
            {
                return std::nullopt;
            }
            return std::string(statement->Text(0));
        }

        Expected<std::vector<std::byte>> Compress(std::string_view text)
        {
            const std::size_t bound = ::ZSTD_compressBound(text.size());
            std::vector<std::byte> out(bound);
            const std::size_t written =
                ::ZSTD_compress(out.data(), out.size(), text.data(), text.size(), kCompressionLevel);
            if (::ZSTD_isError(written) != 0)
            {
                return Fail(DiagnosticCode::Io, Severity::Failed, "HexRaysExport::Compress", "pseudocode",
                            ::ZSTD_getErrorName(written), "report the function's address and its size");
            }
            out.resize(written);
            return out;
        }

        // The plain size travels in its own column: ZSTD_getFrameContentSize can answer UNKNOWN,
        // and a decompression that has to guess its output size is a loop over reallocations.
        Expected<std::string> Decompress(std::span<const std::byte> blob, std::size_t plain)
        {
            std::string out(plain, '\0');
            const std::size_t written = ::ZSTD_decompress(out.data(), out.size(), blob.data(), blob.size());
            if (::ZSTD_isError(written) != 0)
            {
                return Fail(DiagnosticCode::Malformed, Severity::Failed, "HexRaysExport::Decompress", "pseudocode",
                            ::ZSTD_getErrorName(written), "rebuild this image with Sherlock build hexrays");
            }
            out.resize(written);
            return out;
        }
    }

    std::string_view ToText(Status status)
    {
        switch (status)
        {
        case Status::Ok:
            return "Ok";
        case Status::Timeout:
            return "Timeout";
        case Status::Failed:
            return "Failed";
        }
        return "Failed";
    }

    std::optional<Status> FromText(std::string_view text)
    {
        if (text == "Ok")
        {
            return Status::Ok;
        }
        if (text == "Timeout")
        {
            return Status::Timeout;
        }
        if (text == "Failed")
        {
            return Status::Failed;
        }
        return std::nullopt;
    }

    std::filesystem::path StorePath(const std::filesystem::path& storeDir, std::string_view imageName)
    {
        return storeDir / std::format("{}.HexRays.db", imageName);
    }

    Expected<void> CreateStore(Store::Database& db, std::string_view imagePath, std::string_view build,
                               std::string_view idaVersion)
    {
        if (auto ok = db.Execute(kTables); !ok)
        {
            return ok;
        }
        if (auto ok = WriteMeta(db, "Kind", kKind); !ok)
        {
            return ok;
        }
        if (auto ok = WriteMeta(db, "SchemaVersion", std::to_string(kHexRaysSchemaVersion)); !ok)
        {
            return ok;
        }
        if (auto ok = WriteMeta(db, "SherlockVersion", SHERLOCK_VERSION); !ok)
        {
            return ok;
        }
        if (auto ok = WriteMeta(db, "ImagePath", imagePath); !ok)
        {
            return ok;
        }
        if (auto ok = WriteMeta(db, "Build", build); !ok)
        {
            return ok;
        }
        // Created unfinished on purpose. Every call here is the start of a run that intends to
        // reach the last function, and only reaching it writes the 1.
        if (auto ok = WriteMeta(db, "Complete", "0"); !ok)
        {
            return ok;
        }
        // Two IDA versions decompile the same function differently, so a row states which one
        // produced it rather than leaving the reader to assume the installed one.
        return WriteMeta(db, "IdaVersion", idaVersion);
    }

    Expected<void> CheckStoreSchema(Store::Database& db)
    {
        return Store::CheckSchema(db, kKind, kHexRaysSchemaVersion);
    }

    Expected<void> MarkComplete(Store::Database& db)
    {
        return WriteMeta(db, "Complete", "1");
    }

    Expected<bool> IsComplete(Store::Database& db)
    {
        auto value = MetaValue(db, "Complete");
        if (!value)
        {
            return std::unexpected(value.error());
        }
        return !value->has_value() || **value == "1";
    }

    Expected<std::vector<std::uint64_t>> ReadStoredFunctions(Store::Database& db)
    {
        auto statement = db.Prepare("SELECT Function FROM Decompilation");
        if (!statement)
        {
            return std::unexpected(statement.error());
        }
        std::vector<std::uint64_t> out;
        while (true)
        {
            auto row = statement->Step();
            if (!row)
            {
                return std::unexpected(row.error());
            }
            if (!*row)
            {
                break;
            }
            out.push_back(static_cast<std::uint64_t>(statement->Int(0)));
        }
        return out;
    }

    Expected<void> WriteRows(Store::Database& db, const std::vector<DecompilationRow>& rows,
                             const std::vector<NameRow>& names)
    {
        auto transaction = Store::Transaction::Begin(db);
        if (!transaction)
        {
            return std::unexpected(transaction.error());
        }

        auto insert = db.Prepare("INSERT OR REPLACE INTO Decompilation(Function, Pseudocode, Plain, Lines, "
                                 "Status, Reason, Seconds) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
        if (!insert)
        {
            return std::unexpected(insert.error());
        }
        for (const auto& row : rows)
        {
            std::vector<std::byte> blob;
            if (!row.Pseudocode.empty())
            {
                auto compressed = Compress(row.Pseudocode);
                if (!compressed)
                {
                    return std::unexpected(compressed.error());
                }
                blob = std::move(*compressed);
            }

            auto ok = insert->Bind(1, static_cast<std::int64_t>(row.Function));
            if (ok && blob.empty())
            {
                ok = insert->BindNull(2);
            }
            else if (ok)
            {
                ok = insert->BindBlob(2, blob);
            }
            if (ok)
            {
                ok = insert->Bind(3, static_cast<std::int64_t>(row.Pseudocode.size()));
            }
            if (ok)
            {
                ok = insert->Bind(4, static_cast<std::int64_t>(row.Lines));
            }
            if (ok)
            {
                ok = insert->Bind(5, ToText(row.State));
            }
            if (ok)
            {
                ok = row.Reason.empty() ? insert->BindNull(6) : insert->Bind(6, row.Reason);
            }
            if (ok)
            {
                ok = insert->Bind(7, row.Seconds);
            }
            if (!ok)
            {
                return ok;
            }
            if (auto step = insert->Step(); !step)
            {
                return std::unexpected(step.error());
            }
            if (auto reset = insert->Reset(); !reset)
            {
                return reset;
            }
        }

        auto name = db.Prepare("INSERT OR REPLACE INTO IdaName(Address, Name) VALUES(?1, ?2)");
        if (!name)
        {
            return std::unexpected(name.error());
        }
        for (const auto& row : names)
        {
            auto ok = name->Bind(1, static_cast<std::int64_t>(row.Address));
            if (ok)
            {
                ok = name->Bind(2, row.Name);
            }
            if (!ok)
            {
                return ok;
            }
            if (auto step = name->Step(); !step)
            {
                return std::unexpected(step.error());
            }
            if (auto reset = name->Reset(); !reset)
            {
                return reset;
            }
        }

        return transaction->Commit();
    }

    Expected<std::optional<DecompilationRow>> ReadFunction(Store::Database& db, std::uint64_t address)
    {
        auto read = db.Prepare("SELECT Pseudocode, Plain, Lines, Status, Reason, Seconds "
                               "FROM Decompilation WHERE Function = ?1");
        if (!read)
        {
            return std::unexpected(read.error());
        }
        if (auto ok = read->Bind(1, static_cast<std::int64_t>(address)); !ok)
        {
            return std::unexpected(ok.error());
        }
        auto row = read->Step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            return std::nullopt;
        }

        DecompilationRow out;
        out.Function = address;
        const auto plain = static_cast<std::size_t>(read->Int(1));
        if (!read->IsNull(0) && plain != 0)
        {
            auto text = Decompress(read->Blob(0), plain);
            if (!text)
            {
                return std::unexpected(text.error());
            }
            out.Pseudocode = std::move(*text);
        }
        out.Lines = static_cast<std::uint32_t>(read->Int(2));
        if (auto state = FromText(read->Text(3)))
        {
            out.State = *state;
        }
        else
        {
            return Fail(DiagnosticCode::Malformed, Severity::Failed, "HexRaysExport::ReadFunction",
                        Foundation::Hex(address), std::format("unknown status '{}'", read->Text(3)),
                        "rebuild this image with Sherlock build hexrays");
        }
        if (!read->IsNull(4))
        {
            out.Reason = read->Text(4);
        }
        out.Seconds = read->Real(5);
        return out;
    }

    Expected<Coverage> ReadCoverage(Store::Database& db)
    {
        auto decompiled = db.ScalarInt("SELECT count(*) FROM Decompilation WHERE Status = 'Ok'");
        if (!decompiled)
        {
            return std::unexpected(decompiled.error());
        }
        auto attempted = db.ScalarInt("SELECT count(*) FROM Decompilation");
        if (!attempted)
        {
            return std::unexpected(attempted.error());
        }
        auto lines = db.ScalarInt("SELECT coalesce(sum(Lines), 0) FROM Decompilation");
        if (!lines)
        {
            return std::unexpected(lines.error());
        }

        // Counted out of the store rather than carried in a run's own tally: a resumed run
        // attempts only what is missing, so a counter it kept would report the last slice and
        // call it the image.
        auto tooBig = db.Prepare("SELECT count(*) FROM Decompilation WHERE Reason = ?1");
        if (!tooBig)
        {
            return std::unexpected(tooBig.error());
        }
        if (auto ok = tooBig->Bind(1, kTooBigReason); !ok)
        {
            return std::unexpected(ok.error());
        }
        auto row = tooBig->Step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        return Coverage{static_cast<std::uint64_t>(*decompiled), static_cast<std::uint64_t>(*attempted),
                        *row ? static_cast<std::uint64_t>(tooBig->Int(0)) : 0,
                        static_cast<std::uint64_t>(*lines)};
    }
}
