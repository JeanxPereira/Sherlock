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
            std::string   Reason;
            std::string   FactsVersion;
            std::uint64_t CoverageRead  = 0;
            std::uint64_t CoverageTotal = 0;
        };

        Foundation::Expected<std::string> ValidateCatalog(Store::Database& catalog,
                                                           const DyldSharedCache::Cache* cache = nullptr)
        {
            if (auto ok = Store::CheckSchema(catalog, "Catalog"); !ok) return std::unexpected(ok.error());
            const auto uuid = Store::ReadMeta(catalog, "CacheUuid");
            if (!uuid) return std::unexpected(uuid.error());
            if (cache != nullptr && *uuid != cache->Uuid())
            {
                return Foundation::Fail(Foundation::DiagnosticCode::Mismatch, Foundation::Severity::NotVerified,
                                        "ValidateCatalog", catalog.Path().string(),
                                        "the catalog CacheUuid does not match the opened cache",
                                        "use --cache and --store from the same build");
            }
            return *uuid;
        }

        Foundation::Expected<Store::Database> OpenImage(const QueryEnvironment& env, const ImageRow& image,
                                                         std::string_view cacheUuid)
        {
            auto db = Store::Database::Open(env.Store / "Images" / image.Name, Store::Database::Mode::ReadOnly);
            if (!db) return std::unexpected(db.error());
            if (auto ok = Store::CheckSchema(*db, "Image"); !ok) return std::unexpected(ok.error());
            const auto path = Store::ReadMeta(*db, "ImagePath");
            const auto uuid = Store::ReadMeta(*db, "CacheUuid");
            if (!path) return std::unexpected(path.error());
            if (!uuid) return std::unexpected(uuid.error());
            if (*path != image.Path || *uuid != cacheUuid)
            {
                return Foundation::Fail(Foundation::DiagnosticCode::Mismatch, Foundation::Severity::NotVerified,
                                        "OpenImage", db->Path().string(),
                                        "the image store identity does not match its catalog row",
                                        "rebuild this image with Sherlock build facts");
            }
            return db;
        }

        Foundation::Expected<std::vector<ImageRow>> LoadImages(Store::Database& catalog)
        {
            auto statement = catalog.Prepare(
                "SELECT Image.Path, Image.Name, Image.State, "
                "COALESCE(Image.Reason, ''), COALESCE(Image.FactsVersion, ''), "
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
                               std::string(statement->Text(2)), std::string(statement->Text(3)),
                               std::string(statement->Text(4)), static_cast<std::uint64_t>(statement->Int(5)),
                               static_cast<std::uint64_t>(statement->Int(6))});
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

        void Emit(const QueryEnvironment& env, std::string line)
        {
            if (env.Output != nullptr) env.Output->push_back(line);
            if (!env.Json) std::printf("%s\n", line.c_str());
        }

        void PrintCallLine(const QueryEnvironment& env, const std::string& imageBasename, std::int64_t site,
                           std::int64_t caller,
                           std::string_view via, bool islandIsNull, std::int64_t island)
        {
            if (via == "Island" && !islandIsNull)
            {
                Emit(env, std::format("  0x{:x}  in 0x{:x}  {}  via Island 0x{:x}", site, caller,
                                      imageBasename, island));
            }
            else
            {
                Emit(env, std::format("  0x{:x}  in 0x{:x}  {}  via {}", site, caller, imageBasename, via));
            }
        }
    }

    Foundation::Expected<void> PrintHeader(const QueryEnvironment& env, std::string_view build)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return std::unexpected(catalog.error());
        }
        const auto catalogUuid = ValidateCatalog(*catalog);
        if (!catalogUuid) return std::unexpected(catalogUuid.error());
        std::string catalogBuild;
        auto meta = catalog->Prepare("SELECT Value FROM Meta WHERE Key = 'Build'");
        if (!meta)
        {
            return std::unexpected(meta.error());
        }
        const auto buildRow = meta->Step();
        if (!buildRow)
        {
            return std::unexpected(buildRow.error());
        }
        if (!*buildRow || meta->Text(0).empty())
        {
            return Foundation::Fail(Foundation::DiagnosticCode::NotFound, Foundation::Severity::NotVerified,
                                    "Cli::PrintHeader", catalog->Path().string(), "Catalog.Build metadata is missing",
                                    "run Sherlock build facts to populate the catalog");
        }
        catalogBuild = meta->Text(0);
        const std::string_view headerBuild = build.empty() ? std::string_view(catalogBuild) : build;
        const auto rows = LoadImages(*catalog);
        if (!rows)
        {
            return std::unexpected(rows.error());
        }
        std::size_t   done  = 0;
        std::uint64_t read  = 0;
        std::uint64_t total = 0;
        for (const auto& row : *rows)
        {
            if (row.State == "FactsDone")
            {
                ++done;
                read  += row.CoverageRead;
                total += row.CoverageTotal;
            }
        }
        Emit(env, std::format("Sherlock {} \xC2\xB7 build {} \xC2\xB7 layer 1 facts \xC2\xB7 {} image(s) \xC2\xB7 coverage {}/{} instructions",
                              SHERLOCK_VERSION, headerBuild, done, read, total));
        return {};
    }

    Verdict RunCallers(const QueryEnvironment& env, std::uint64_t address)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return {VerdictKind::NotVerified, 0, catalog.error().Format(), 0, 0};
        }
        const auto catalogUuid = ValidateCatalog(*catalog);
        if (!catalogUuid) return {VerdictKind::NotVerified, 0, catalogUuid.error().Format(), 0, 0};
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
            auto db = OpenImage(env, image, *catalogUuid);
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
                    PrintCallLine(env, Basename(image.Path), statement->Int(0), statement->Int(1), statement->Text(2),
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
                return Owned{ImageRow{owner->Image->Path, {}, "Pending", {}, {}, 0, 0}, std::string(owner->Segment)};
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
        const auto catalogUuid = ValidateCatalog(*catalog, &cache);
        if (!catalogUuid) return {VerdictKind::NotVerified, 0, catalogUuid.error().Format(), 0, 0};
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
                auto db = OpenImage(env, image, *catalogUuid);
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
            Emit(env, "owner: (global cache data)");
            return {VerdictKind::Empty, 0, {}, 0, 0};
        }
        Emit(env, std::format("owner: {} {}", owned->Image.Path, owned->Segment));
        if (owned->Image.State != "FactsDone")
        {
            return {VerdictKind::Partial, 0, std::format("image {} not built", owned->Image.Path), 0, 0};
        }

        auto db = OpenImage(env, owned->Image, *catalogUuid);
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
        Emit(env, std::format("function: 0x{:x} (+0x{:x})", *functionAddress,
                              static_cast<std::uint64_t>(*address) - static_cast<std::uint64_t>(*functionAddress)));

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
            Emit(env, std::format("symbol: {}", sym->Text(0)));
            if (!sym->IsNull(1))
            {
                Emit(env, std::format("demangled: {}", sym->Text(1)));
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
                Emit(env, std::format("  callee  0x{:x}  -> 0x{:x}  via {}", callees->Int(0), callees->Int(1),
                                      callees->Text(2)));
            }
        }
        Emit(env, std::format("callees: {}", calleeCount));

        const auto callerVerdict = RunCallers(env, static_cast<std::uint64_t>(*functionAddress));
        if (callerVerdict.Kind == VerdictKind::NotVerified)
        {
            return callerVerdict;
        }
        Emit(env, std::format("callers: {}", callerVerdict.Count));

        // q's caller summary scans every FactsDone store, including the owning image.
        // Its coverage therefore represents the full query scope and counts each image once.
        return {VerdictKind::Found, 1, {}, callerVerdict.Read, callerVerdict.Total};
    }

    Verdict RunCalls(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t address)
    {
        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return {VerdictKind::NotVerified, 0, catalog.error().Format(), 0, 0};
        }
        const auto catalogUuid = ValidateCatalog(*catalog, &cache);
        if (!catalogUuid) return {VerdictKind::NotVerified, 0, catalogUuid.error().Format(), 0, 0};
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
            Emit(env, "owner: (global cache data)");
            return {VerdictKind::Empty, 0, {}, 0, 0};
        }
        if (owned->Image.State != "FactsDone")
        {
            return {VerdictKind::Partial, 0, std::format("image {} not built", owned->Image.Path), 0, 0};
        }
        auto db = OpenImage(env, owned->Image, *catalogUuid);
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
                Emit(env, std::format("  0x{:x}  -> 0x{:x}  via {}", statement->Int(0), statement->Int(1),
                                      statement->Text(2)));
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
        const auto catalogUuid = ValidateCatalog(*catalog, &cache);
        if (!catalogUuid) return {VerdictKind::NotVerified, 0, catalogUuid.error().Format(), 0, 0};
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
            Emit(env, "owner: (global cache data)");
            return {VerdictKind::Empty, 0, {}, 0, 0};
        }
        if (owned->Image.State != "FactsDone")
        {
            return {VerdictKind::Partial, 0, std::format("image {} not built", owned->Image.Path), 0, 0};
        }
        auto db = OpenImage(env, owned->Image, *catalogUuid);
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
            if ((env.Full || count <= 10) && statement->IsNull(3))
            {
                Emit(env, std::format("  0x{:x}  -> 0x{:x}  {}", statement->Int(0), statement->Int(1),
                                      statement->Text(2)));
            }
            else if (env.Full || count <= 10)
            {
                Emit(env, std::format("  0x{:x}  -> 0x{:x}  {}  (f64 {})", statement->Int(0), statement->Int(1),
                                      statement->Text(2), statement->Real(3)));
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
        const auto catalogUuid = ValidateCatalog(*catalog);
        if (!catalogUuid) return {VerdictKind::NotVerified, 0, catalogUuid.error().Format(), 0, 0};
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
        const auto schema = Store::ReadMeta(*catalog, "SchemaVersion");
        const auto producing = Store::ReadMeta(*catalog, "SherlockVersion");
        if (!schema) return {VerdictKind::NotVerified, 0, schema.error().Format(), 0, 0};
        if (!producing) return {VerdictKind::NotVerified, 0, producing.error().Format(), 0, 0};
        Emit(env, std::format("schema: {}", *schema));
        Emit(env, std::format("produced by Sherlock: {}", *producing));
        Emit(env, std::format("running Sherlock: {}", SHERLOCK_VERSION));
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
                Emit(env, std::format("{}: {}", key, meta->Text(0)));
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
            Emit(env, std::format("  {:<12} {:<40} facts {} coverage {}/{}{}{}", image.State, image.Path,
                                  image.FactsVersion.empty() ? "(none)" : image.FactsVersion,
                                  image.CoverageRead, image.CoverageTotal,
                                  image.Reason.empty() ? "" : " reason: ", image.Reason));
            if (image.State == "FactsDone")
            {
                ++done;
                read += image.CoverageRead;
                total += image.CoverageTotal;
            }
        }
        std::uintmax_t diskBytes = 0;
        std::error_code diskError;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(env.Store, diskError))
        {
            if (entry.is_regular_file(diskError)) diskBytes += entry.file_size(diskError);
            if (diskError) break;
        }
        if (diskError)
            return {VerdictKind::NotVerified, 0, diskError.message(), read, total};
        Emit(env, std::format("disk: {} bytes", diskBytes));
        Emit(env, "layer 2: not built");
        Emit(env, "layer 3: not built");
        return {VerdictKind::Found, done, {}, read, total};
    }
}
