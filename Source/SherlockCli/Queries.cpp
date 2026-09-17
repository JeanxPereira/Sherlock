// Sherlock — tools/Sherlock/Source/SherlockCli/Queries.cpp
// Per-image-store SQL behind every subcommand, routed through DyldSharedCache::Cache::Owner.
#include <SherlockCli/Queries.h>

#include <Store/Database.h>
#include <Store/Schema.h>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Sherlock::Cli
{
    namespace
    {
        struct ImageRow
        {
            std::string   Path;
            std::string   Name; // the store's own file name, exactly as Task 6 wrote it
            std::string   State;
            std::uint64_t CoverageRead  = 0;
            std::uint64_t CoverageTotal = 0;
        };

        Foundation::Expected<std::vector<ImageRow>> LoadImages(Store::Database& catalog)
        {
            auto statement = catalog.Prepare(
                "SELECT Image.Path, Image.Name, Image.State, "
                "COALESCE(Coverage.Read, 0), COALESCE(Coverage.Total, 0) "
                "FROM Image LEFT JOIN Coverage ON Coverage.Image = Image.Path AND Coverage.Layer = 'Facts'");
            if (!statement)
            {
                return std::unexpected(statement.error());
            }
            std::vector<ImageRow> rows;
            for (;;)
            {
                const auto row = statement->Step();
                if (!row)
                {
                    return std::unexpected(row.error());
                }
                if (!*row)
                {
                    break;
                }
                rows.push_back({std::string(statement->Text(0)), std::string(statement->Text(1)),
                               std::string(statement->Text(2)), static_cast<std::uint64_t>(statement->Int(3)),
                               static_cast<std::uint64_t>(statement->Int(4))});
            }
            return rows;
        }

        std::optional<ImageRow> FindImageRow(const std::vector<ImageRow>& rows, std::string_view path)
        {
            for (const auto& row : rows)
            {
                if (row.Path == path)
                {
                    return row;
                }
            }
            return std::nullopt;
        }

        std::string Basename(std::string_view path)
        {
            const auto slash = path.rfind('/');
            return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
        }

        std::optional<std::uint64_t> ParseAddress(std::string_view text)
        {
            if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
            {
                return std::nullopt;
            }
            char*                dummy    = nullptr;
            const std::string    owned(text);
            const std::uint64_t  value    = std::strtoull(owned.c_str(), &dummy, 16);
            return dummy != nullptr && *dummy == '\0' ? std::optional<std::uint64_t>(value) : std::nullopt;
        }

        void PrintCallLine(const std::string& imageBasename, std::int64_t site, std::int64_t caller,
                           std::string_view via, bool islandIsNull, std::int64_t island)
        {
            if (via == "Island" && !islandIsNull)
            {
                std::printf("  0x%llx  in 0x%llx  %s  via Island 0x%llx\n", static_cast<unsigned long long>(site),
                           static_cast<unsigned long long>(caller), imageBasename.c_str(),
                           static_cast<unsigned long long>(island));
            }
            else
            {
                std::printf("  0x%llx  in 0x%llx  %s  via %.*s\n", static_cast<unsigned long long>(site),
                           static_cast<unsigned long long>(caller), imageBasename.c_str(), static_cast<int>(via.size()),
                           via.data());
            }
        }
    }

    void PrintHeader(const QueryEnvironment& env, std::string_view build)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            std::printf("Sherlock %s . build %.*s . layer 1 facts . catalog unavailable\n", SHERLOCK_VERSION,
                       static_cast<int>(build.size()), build.data());
            return;
        }
        std::string catalogBuild;
        if (auto meta = catalog->Prepare("SELECT Value FROM Meta WHERE Key = 'Build'"); meta)
        {
            const auto row = meta->Step();
            if (row && *row)
            {
                catalogBuild = meta->Text(0);
            }
        }
        const std::string_view headerBuild = build.empty() ? std::string_view(catalogBuild) : build;
        const auto rows = LoadImages(*catalog);
        std::size_t   done  = 0;
        std::uint64_t read  = 0;
        std::uint64_t total = 0;
        if (rows)
        {
            for (const auto& row : *rows)
            {
                if (row.State == "FactsDone")
                {
                    ++done;
                    read  += row.CoverageRead;
                    total += row.CoverageTotal;
                }
            }
        }
        std::printf("Sherlock %s \xC2\xB7 build %.*s \xC2\xB7 layer 1 facts \xC2\xB7 %zu image(s) \xC2\xB7 coverage "
                   "%llu/%llu instructions\n",
                   SHERLOCK_VERSION, static_cast<int>(headerBuild.size()), headerBuild.data(), done,
                   static_cast<unsigned long long>(read), static_cast<unsigned long long>(total));
    }

    Verdict RunCallers(const QueryEnvironment& env, std::uint64_t address)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return {VerdictKind::NotVerified, 0, catalog.error().Format(), 0, 0};
        }
        const auto rows = LoadImages(*catalog);
        if (!rows)
        {
            return {VerdictKind::NotVerified, 0, rows.error().Format(), 0, 0};
        }

        std::size_t   count = 0;
        std::uint64_t read = 0, total = 0;
        for (const auto& image : *rows)
        {
            if (image.State != "FactsDone")
            {
                continue;
            }
            auto db = Store::Database::Open(env.Store / "Images" / image.Name, Store::Database::Mode::ReadOnly);
            if (!db)
            {
                return {VerdictKind::NotVerified, 0, db.error().Format(), read, total};
            }
            read += image.CoverageRead;
            total += image.CoverageTotal;
            auto statement = db->Prepare("SELECT Site, Caller, Via, Island FROM Call WHERE Target = ?1 ORDER BY Site");
            if (!statement)
            {
                return {VerdictKind::NotVerified, 0, statement.error().Format(), read, total};
            }
            if (auto bind = statement->Bind(1, static_cast<std::int64_t>(address)); !bind)
            {
                return {VerdictKind::NotVerified, 0, bind.error().Format(), read, total};
            }
            for (;;)
            {
                const auto row = statement->Step();
                if (!row)
                {
                    return {VerdictKind::NotVerified, 0, row.error().Format(), read, total};
                }
                if (!*row)
                {
                    break;
                }
                ++count;
                if (env.Full || count <= 10)
                {
                    PrintCallLine(Basename(image.Path), statement->Int(0), statement->Int(1), statement->Text(2),
                                 statement->IsNull(3), statement->Int(3));
                }
            }
        }
        return {count > 0 ? VerdictKind::Found : VerdictKind::Empty, count, {}, read, total};
    }

    namespace
    {
        struct Owned
        {
            ImageRow      Image;
            std::string   Segment;
        };

        // Resolves cache.Owner(address) against the catalog's own rows; nullopt means no image owns the address.
        std::optional<Owned> Resolve(const DyldSharedCache::Cache& cache, const std::vector<ImageRow>& rows,
                                     std::uint64_t address)
        {
            const auto owner = cache.Owner(address);
            if (!owner)
            {
                return std::nullopt;
            }
            const auto row = FindImageRow(rows, owner->Image->Path);
            if (!row)
            {
                return Owned{ImageRow{owner->Image->Path, {}, "Pending", 0, 0}, std::string(owner->Segment)};
            }
            return Owned{*row, std::string(owner->Segment)};
        }
    }

    Verdict RunQuery(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::string_view target)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return {VerdictKind::NotVerified, 0, catalog.error().Format(), 0, 0};
        }
        const auto rows = LoadImages(*catalog);
        if (!rows)
        {
            return {VerdictKind::NotVerified, 0, rows.error().Format(), 0, 0};
        }

        std::optional<std::uint64_t> address = ParseAddress(target);
        if (!address)
        {
            std::uint64_t read = 0, total = 0;
            for (const auto& image : *rows)
            {
                if (image.State != "FactsDone")
                {
                    continue;
                }
                auto db = Store::Database::Open(env.Store / "Images" / image.Name, Store::Database::Mode::ReadOnly);
                if (!db)
                {
                    return {VerdictKind::NotVerified, 0, db.error().Format(), read, total};
                }
                read += image.CoverageRead;
                total += image.CoverageTotal;
                auto statement = db->Prepare("SELECT Symbol.Address FROM Symbol JOIN Name ON Name.Id = Symbol.Name "
                                             "WHERE Name.Text = ?1 "
                                             "UNION SELECT Symbol.Address FROM Symbol JOIN Name "
                                             "ON Name.Id = Symbol.Demangled WHERE Name.Text = ?1 LIMIT 1");
                if (!statement)
                {
                    return {VerdictKind::NotVerified, 0, statement.error().Format(), read, total};
                }
                if (auto bind = statement->Bind(1, target); !bind)
                {
                    return {VerdictKind::NotVerified, 0, bind.error().Format(), read, total};
                }
                const auto row = statement->Step();
                if (!row)
                {
                    return {VerdictKind::NotVerified, 0, row.error().Format(), read, total};
                }
                if (*row)
                {
                    address = static_cast<std::uint64_t>(statement->Int(0));
                    break;
                }
            }
            if (!address)
            {
                return {VerdictKind::Empty, 0, {}, read, total};
            }
        }

        if (!cache.IsMapped(*address))
        {
            return {VerdictKind::NotVerified, 0, std::format("0x{:x} is not mapped", *address), 0, 0};
        }
        const auto owned = Resolve(cache, *rows, *address);
        if (!owned)
        {
            std::printf("owner: (global cache data)\n");
            return {VerdictKind::Empty, 0, {}, 0, 0};
        }
        std::printf("owner: %s %s\n", owned->Image.Path.c_str(), owned->Segment.c_str());
        if (owned->Image.State != "FactsDone")
        {
            return {VerdictKind::Partial, 0, std::format("image {} not built", owned->Image.Path), 0, 0};
        }

        auto db = Store::Database::Open(env.Store / "Images" / owned->Image.Name, Store::Database::Mode::ReadOnly);
        if (!db)
        {
            return {VerdictKind::NotVerified, 0, db.error().Format(), 0, 0};
        }
        auto fn = db->Prepare("SELECT Address, Size FROM Function WHERE Address <= ?1 ORDER BY Address DESC LIMIT 1");
        if (!fn)
        {
            return {VerdictKind::NotVerified, 0, fn.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (auto bind = fn->Bind(1, static_cast<std::int64_t>(*address)); !bind)
        {
            return {VerdictKind::NotVerified, 0, bind.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        std::optional<std::int64_t> functionAddress;
        const auto functionRow = fn->Step();
        if (!functionRow)
        {
            return {VerdictKind::NotVerified, 0, functionRow.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (*functionRow && *address < static_cast<std::uint64_t>(fn->Int(0)) + static_cast<std::uint64_t>(fn->Int(1)))
        {
            functionAddress = fn->Int(0);
        }
        if (!functionAddress)
        {
            return {VerdictKind::Empty, 0, {}, owned->Image.CoverageRead, owned->Image.CoverageTotal};
        }
        std::printf("function: 0x%llx (+0x%llx)\n", static_cast<unsigned long long>(*functionAddress),
                   static_cast<unsigned long long>(*address) - static_cast<unsigned long long>(*functionAddress));

        auto sym = db->Prepare("SELECT N.Text, D.Text FROM Symbol S JOIN Name N ON N.Id = S.Name "
                               "LEFT JOIN Name D ON D.Id = S.Demangled WHERE S.Address = ?1 LIMIT 1");
        if (!sym)
        {
            return {VerdictKind::NotVerified, 0, sym.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (auto bind = sym->Bind(1, *functionAddress); !bind)
        {
            return {VerdictKind::NotVerified, 0, bind.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        const auto symbolRow = sym->Step();
        if (!symbolRow)
        {
            return {VerdictKind::NotVerified, 0, symbolRow.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (*symbolRow)
        {
            std::printf("symbol: %.*s\n", static_cast<int>(sym->Text(0).size()), sym->Text(0).data());
            if (!sym->IsNull(1))
            {
                std::printf("demangled: %.*s\n", static_cast<int>(sym->Text(1).size()), sym->Text(1).data());
            }
        }

        auto callees = db->Prepare("SELECT Site, Target, Via FROM Call WHERE Caller = ?1 ORDER BY Site");
        if (!callees)
        {
            return {VerdictKind::NotVerified, 0, callees.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (auto bind = callees->Bind(1, *functionAddress); !bind)
        {
            return {VerdictKind::NotVerified, 0, bind.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        std::size_t calleeCount = 0;
        for (;;)
        {
            const auto row = callees->Step();
            if (!row)
            {
                return {VerdictKind::NotVerified, 0, row.error().Format(), owned->Image.CoverageRead,
                        owned->Image.CoverageTotal};
            }
            if (!*row) break;
            ++calleeCount;
            if (env.Full || calleeCount <= 10)
            {
                std::printf("  callee  0x%llx  -> 0x%llx  via %.*s\n",
                           static_cast<unsigned long long>(callees->Int(0)),
                           static_cast<unsigned long long>(callees->Int(1)), static_cast<int>(callees->Text(2).size()),
                           callees->Text(2).data());
            }
        }
        std::printf("callees: %zu\n", calleeCount);

        return {VerdictKind::Found, 1, {}, owned->Image.CoverageRead, owned->Image.CoverageTotal};
    }

    Verdict RunCalls(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t address)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return {VerdictKind::NotVerified, 0, catalog.error().Format(), 0, 0};
        }
        const auto rows = LoadImages(*catalog);
        if (!rows)
        {
            return {VerdictKind::NotVerified, 0, rows.error().Format(), 0, 0};
        }
        const auto owned = Resolve(cache, *rows, address);
        if (!owned)
        {
            if (!cache.IsMapped(address))
            {
                return {VerdictKind::NotVerified, 0, std::format("0x{:x} is not mapped", address), 0, 0};
            }
            std::printf("owner: (global cache data)\n");
            return {VerdictKind::Empty, 0, {}, 0, 0};
        }
        if (owned->Image.State != "FactsDone")
        {
            return {VerdictKind::Partial, 0, std::format("image {} not built", owned->Image.Path), 0, 0};
        }
        auto db = Store::Database::Open(env.Store / "Images" / owned->Image.Name, Store::Database::Mode::ReadOnly);
        if (!db)
        {
            return {VerdictKind::NotVerified, 0, db.error().Format(), 0, 0};
        }
        auto isFunction = db->Prepare("SELECT 1 FROM Function WHERE Address = ?1");
        if (!isFunction)
        {
            return {VerdictKind::NotVerified, 0, isFunction.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (auto bind = isFunction->Bind(1, static_cast<std::int64_t>(address)); !bind)
        {
            return {VerdictKind::NotVerified, 0, bind.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        const auto isRow = isFunction->Step();
        if (!isRow)
        {
            return {VerdictKind::NotVerified, 0, isRow.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (!*isRow)
        {
            return {VerdictKind::NotVerified, 0, std::format("0x{:x} is not a function start", address), 0, 0};
        }

        auto statement = db->Prepare("SELECT Site, Target, Via FROM Call WHERE Caller = ?1 ORDER BY Site");
        if (!statement)
        {
            return {VerdictKind::NotVerified, 0, statement.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (auto bind = statement->Bind(1, static_cast<std::int64_t>(address)); !bind)
        {
            return {VerdictKind::NotVerified, 0, bind.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        std::size_t count = 0;
        for (;;)
        {
            const auto row = statement->Step();
            if (!row)
            {
                return {VerdictKind::NotVerified, 0, row.error().Format(), owned->Image.CoverageRead,
                        owned->Image.CoverageTotal};
            }
            if (!*row) break;
            ++count;
            if (env.Full || count <= 10)
            {
                std::printf("  0x%llx  -> 0x%llx  via %.*s\n", static_cast<unsigned long long>(statement->Int(0)),
                           static_cast<unsigned long long>(statement->Int(1)), static_cast<int>(statement->Text(2).size()),
                           statement->Text(2).data());
            }
        }
        return {count > 0 ? VerdictKind::Found : VerdictKind::Empty, count, {}, owned->Image.CoverageRead,
               owned->Image.CoverageTotal};
    }

    Verdict RunRefs(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t address,
                    std::uint64_t end)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return {VerdictKind::NotVerified, 0, catalog.error().Format(), 0, 0};
        }
        const auto rows = LoadImages(*catalog);
        if (!rows)
        {
            return {VerdictKind::NotVerified, 0, rows.error().Format(), 0, 0};
        }
        const auto owned = Resolve(cache, *rows, address);
        if (!owned)
        {
            if (!cache.IsMapped(address))
            {
                return {VerdictKind::NotVerified, 0, std::format("0x{:x} is not mapped", address), 0, 0};
            }
            std::printf("owner: (global cache data)\n");
            return {VerdictKind::Empty, 0, {}, 0, 0};
        }
        if (owned->Image.State != "FactsDone")
        {
            return {VerdictKind::Partial, 0, std::format("image {} not built", owned->Image.Path), 0, 0};
        }
        auto db = Store::Database::Open(env.Store / "Images" / owned->Image.Name, Store::Database::Mode::ReadOnly);
        if (!db)
        {
            return {VerdictKind::NotVerified, 0, db.error().Format(), 0, 0};
        }
        auto statement =
            db->Prepare("SELECT Site, Target, Kind, Value FROM LiteralRef WHERE Target >= ?1 AND Target < ?2 ORDER BY Site");
        if (!statement)
        {
            return {VerdictKind::NotVerified, 0, statement.error().Format(), 0, 0};
        }
        if (auto bind = statement->Bind(1, static_cast<std::int64_t>(address)); !bind)
        {
            return {VerdictKind::NotVerified, 0, bind.error().Format(), 0, 0};
        }
        if (auto bind = statement->Bind(2, static_cast<std::int64_t>(end)); !bind)
        {
            return {VerdictKind::NotVerified, 0, bind.error().Format(), 0, 0};
        }
        std::size_t count = 0;
        for (;;)
        {
            const auto row = statement->Step();
            if (!row)
            {
                return {VerdictKind::NotVerified, 0, row.error().Format(), owned->Image.CoverageRead,
                        owned->Image.CoverageTotal};
            }
            if (!*row) break;
            ++count;
            if (statement->IsNull(3))
            {
                std::printf("  0x%llx  -> 0x%llx  %.*s\n", static_cast<unsigned long long>(statement->Int(0)),
                           static_cast<unsigned long long>(statement->Int(1)), static_cast<int>(statement->Text(2).size()),
                           statement->Text(2).data());
            }
            else
            {
                std::printf("  0x%llx  -> 0x%llx  %.*s  (f64 %g)\n", static_cast<unsigned long long>(statement->Int(0)),
                           static_cast<unsigned long long>(statement->Int(1)), static_cast<int>(statement->Text(2).size()),
                           statement->Text(2).data(), statement->Real(3));
            }
        }
        return {count > 0 ? VerdictKind::Found : VerdictKind::Empty, count, {}, owned->Image.CoverageRead,
               owned->Image.CoverageTotal};
    }

    Verdict RunStatus(const QueryEnvironment& env)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return {VerdictKind::NotVerified, 0, catalog.error().Format(), 0, 0};
        }
        auto meta = catalog->Prepare("SELECT Value FROM Meta WHERE Key = ?1");
        if (!meta)
        {
            return {VerdictKind::NotVerified, 0, meta.error().Format(), 0, 0};
        }
        const auto rows = LoadImages(*catalog);
        if (!rows)
        {
            return {VerdictKind::NotVerified, 0, rows.error().Format(), 0, 0};
        }
        std::printf("schema: %d\n", Store::kSchemaVersion);
        std::printf("Sherlock: %s\n", SHERLOCK_VERSION);
        for (const char* key : {"Build", "CacheUuid"})
        {
            if (auto bind = meta->Bind(1, std::string_view(key)); !bind)
            {
                return {VerdictKind::NotVerified, 0, bind.error().Format(), 0, 0};
            }
            const auto row = meta->Step();
            if (!row)
            {
                return {VerdictKind::NotVerified, 0, row.error().Format(), 0, 0};
            }
            if (*row)
            {
                std::printf("%s: %.*s\n", key, static_cast<int>(meta->Text(0).size()), meta->Text(0).data());
            }
            if (auto reset = meta->Reset(); !reset)
            {
                return {VerdictKind::NotVerified, 0, reset.error().Format(), 0, 0};
            }
        }
        std::uint64_t read = 0, total = 0;
        std::size_t   done = 0;
        for (const auto& image : *rows)
        {
            std::printf("  %-12s %-40s coverage %llu/%llu\n", image.State.c_str(), image.Path.c_str(),
                       static_cast<unsigned long long>(image.CoverageRead),
                       static_cast<unsigned long long>(image.CoverageTotal));
            if (image.State == "FactsDone")
            {
                ++done;
                read += image.CoverageRead;
                total += image.CoverageTotal;
            }
        }
        return {VerdictKind::Found, done, {}, read, total};
    }
}
