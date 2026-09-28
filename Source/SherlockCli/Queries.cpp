// Sherlock — tools/Sherlock/Source/SherlockCli/Queries.cpp
// Per-image-store SQL behind every subcommand, routed through DyldSharedCache::Cache::Owner.
#include <SherlockCli/Queries.h>

#include <DocumentIndex/Builder.h>
#include <DocumentIndex/FileStamp.h>
#include <Facts/Disassembler.h>
#include <HexRaysExport/Store.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

        // ASCII only, and deliberately: an identifier in pseudocode is ASCII, and a locale-aware
        // fold would make the same search answer differently on two machines.
        std::string Lowered(std::string_view text)
        {
            std::string out(text);
            for (char& c : out)
            {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return out;
        }

        std::vector<std::string_view> SplitLines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            while (!text.empty())
            {
                const auto end = text.find('\n');
                if (end == std::string_view::npos)
                {
                    lines.push_back(text);
                    break;
                }
                lines.push_back(text.substr(0, end));
                text.remove_prefix(end + 1);
            }
            return lines;
        }

        // What layer 2 prints as MEMORY[0x...] is a branch island, and layer 1 recorded the
        // island BESIDE the target it jumps to -- Call.Island. So the name the carved slice lost
        // is not lost to the store: it is one join away, and the only reason a reader ever had to
        // run a second instrument was that nothing performed it.
        struct ResolvedIsland
        {
            std::uint64_t Target = 0;
            std::string   Image;      // empty when no image's segments contain the target
            std::string   Symbol;     // empty when the owning image has no symbol at it
            std::string   Demangled;
        };

        Foundation::Expected<std::map<std::uint64_t, ResolvedIsland>>
        ResolveIslands(const QueryEnvironment& env, Store::Database& owner,
                       const std::vector<ImageRow>& rows, std::string_view cacheUuid,
                       std::uint64_t start, std::uint64_t end)
        {
            std::map<std::uint64_t, ResolvedIsland> resolved;
            auto calls = owner.Prepare("SELECT DISTINCT Island, Target FROM Call "
                                       "WHERE Caller = ?1 AND Island IS NOT NULL");
            if (!calls)
            {
                return std::unexpected(calls.error());
            }
            if (auto ok = calls->Bind(1, static_cast<std::int64_t>(start)); !ok)
            {
                return std::unexpected(ok.error());
            }
            (void)end;
            for (;;)
            {
                auto row = calls->Step();
                if (!row)
                {
                    return std::unexpected(row.error());
                }
                if (!*row)
                {
                    break;
                }
                resolved[static_cast<std::uint64_t>(calls->Int(0))] =
                    ResolvedIsland{static_cast<std::uint64_t>(calls->Int(1)), {}, {}, {}};
            }
            if (resolved.empty())
            {
                return resolved;
            }

            // One pass per image rather than one per target: every store is opened at most once,
            // and an image whose store will not open is skipped rather than failing the query --
            // a name that cannot be resolved stays MEMORY[0x...] and is counted as unresolved.
            for (const auto& image : rows)
            {
                bool wanted = false;
                for (const auto& [island, entry] : resolved)
                {
                    (void)island;
                    if (entry.Image.empty())
                    {
                        wanted = true;
                        break;
                    }
                }
                if (!wanted)
                {
                    break;
                }
                auto db = OpenImage(env, image, cacheUuid);
                if (!db)
                {
                    continue;
                }
                auto inside = db->Prepare("SELECT 1 FROM Segment WHERE ?1 >= Address "
                                          "AND ?1 < Address + Size LIMIT 1");
                auto named = db->Prepare("SELECT N.Text, D.Text FROM Symbol S "
                                         "JOIN Name N ON N.Id = S.Name "
                                         "LEFT JOIN Name D ON D.Id = S.Demangled "
                                         "WHERE S.Address = ?1 LIMIT 1");
                if (!inside || !named)
                {
                    continue;
                }
                const auto basename = std::filesystem::path(image.Path).filename().string();
                for (auto& [island, entry] : resolved)
                {
                    (void)island;
                    if (!entry.Image.empty())
                    {
                        continue;
                    }
                    if (!inside->Bind(1, static_cast<std::int64_t>(entry.Target)) ||
                        !named->Bind(1, static_cast<std::int64_t>(entry.Target)))
                    {
                        continue;
                    }
                    auto hit = inside->Step();
                    const bool contained = hit && *hit;
                    (void)inside->Reset();
                    if (!contained)
                    {
                        (void)named->Reset();
                        continue;
                    }
                    entry.Image = basename;
                    if (auto symbol = named->Step(); symbol && *symbol)
                    {
                        entry.Symbol = std::string(named->Text(0));
                        if (!named->IsNull(1))
                        {
                            entry.Demangled = std::string(named->Text(1));
                        }
                    }
                    (void)named->Reset();
                }
            }
            return resolved;
        }

        // Every island of one image at once, for a search rather than for one function. Built
        // from layer 1 and the other images' symbol tables, so a name defined in ANOTHER image
        // becomes findable -- which is the blind spot a text search over a carved slice has by
        // construction, and the reason `grep ColorScheme` found nothing in the image that
        // initializes ColorScheme.dark.
        Foundation::Expected<std::map<std::uint64_t, std::string>>
        IslandSymbols(const QueryEnvironment& env, const std::vector<ImageRow>& rows,
                      std::string_view cacheUuid, Store::Database& source)
        {
            std::map<std::uint64_t, std::string> names;
            std::map<std::uint64_t, std::uint64_t> targets;  // island -> target
            auto islands = source.Prepare("SELECT DISTINCT Island, Target FROM Call "
                                          "WHERE Island IS NOT NULL");
            if (!islands)
            {
                return std::unexpected(islands.error());
            }
            for (;;)
            {
                auto row = islands->Step();
                if (!row)
                {
                    return std::unexpected(row.error());
                }
                if (!*row)
                {
                    break;
                }
                targets[static_cast<std::uint64_t>(islands->Int(0))] =
                    static_cast<std::uint64_t>(islands->Int(1));
            }
            if (targets.empty())
            {
                return names;
            }

            for (const auto& image : rows)
            {
                auto db = OpenImage(env, image, cacheUuid);
                if (!db)
                {
                    continue;
                }
                // The segments first and in memory: a range test per target against a handful of
                // rows costs nothing, where a query per target per image would be 18 times the
                // work for the same answer.
                std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
                if (auto segments = db->Prepare("SELECT Address, Size FROM Segment"))
                {
                    for (;;)
                    {
                        auto row = segments->Step();
                        if (!row || !*row)
                        {
                            break;
                        }
                        const auto start = static_cast<std::uint64_t>(segments->Int(0));
                        spans.emplace_back(start, start + static_cast<std::uint64_t>(segments->Int(1)));
                    }
                }
                if (spans.empty())
                {
                    continue;
                }
                auto named = db->Prepare("SELECT N.Text FROM Symbol S JOIN Name N ON N.Id = S.Name "
                                         "WHERE S.Address = ?1 LIMIT 1");
                if (!named)
                {
                    continue;
                }
                for (const auto& [island, target] : targets)
                {
                    if (names.contains(island))
                    {
                        continue;
                    }
                    bool inside = false;
                    for (const auto& [from, to] : spans)
                    {
                        if (target >= from && target < to)
                        {
                            inside = true;
                            break;
                        }
                    }
                    if (!inside || !named->Bind(1, static_cast<std::int64_t>(target)))
                    {
                        continue;
                    }
                    if (auto row = named->Step(); row && *row)
                    {
                        names[island] = std::string(named->Text(0));
                    }
                    (void)named->Reset();
                }
            }
            return names;
        }

        // A call whose target falls in no segment of this image leaves the image. Layer 2's
        // pseudocode cannot name those -- the slice IDA decompiled does not contain them -- and
        // layer 1, which read the whole cache, can.
        Foundation::Expected<std::int64_t> CallsLeavingImage(Store::Database& db, std::uint64_t start,
                                                             std::uint64_t end)
        {
            auto statement = db.Prepare(
                "SELECT count(*) FROM Call c WHERE c.Site >= ?1 AND c.Site < ?2 AND NOT EXISTS "
                "(SELECT 1 FROM Segment s WHERE c.Target >= s.Address AND c.Target < s.Address + s.Size)");
            if (!statement)
            {
                return std::unexpected(statement.error());
            }
            if (auto ok = statement->Bind(1, static_cast<std::int64_t>(start)); !ok)
            {
                return std::unexpected(ok.error());
            }
            if (auto ok = statement->Bind(2, static_cast<std::int64_t>(end)); !ok)
            {
                return std::unexpected(ok.error());
            }
            const auto row = statement->Step();
            if (!row)
            {
                return std::unexpected(row.error());
            }
            return *row ? statement->Int(0) : 0;
        }

        void Emit(const QueryEnvironment& env, std::string line)
        {
            if (env.Output != nullptr) env.Output->push_back(line);
            if (!env.Json) std::printf("%s\n", line.c_str());
        }

        // The exact `Sherlock build docs` invocation that would populate Documents.db from this
        // environment's own already-resolved --repo/--documents (Arguments.cpp's default,
        // <repo>/build/Sherlock/Documents.db, when --documents was never explicit). Documents.db
        // stays worktree-relative on purpose (the phase-2 plan's own "it follows the worktree"
        // decision: layer 3 is derived from THIS working tree, not from the shared corpus store),
        // so the fix here is not a cleverer default -- it is naming the one command that builds it,
        // instead of an agent spending calls hunting for a path (finding 1).
        std::string BuildDocsCommand(const QueryEnvironment& env)
        {
            const std::string repoArg = env.Repo.empty() ? std::string("<repo>") : env.Repo.string();
            const std::filesystem::path fallbackDocuments =
                env.Repo.empty() ? std::filesystem::path("<repo>/build/Sherlock/Documents.db")
                                 : env.Repo / "build" / "Sherlock" / "Documents.db";
            const std::string docsArg = env.Documents.empty() ? fallbackDocuments.string() : env.Documents.string();
            return std::format("Sherlock build docs --repo {} --documents {}", repoArg, docsArg);
        }

        void EmitLayerThreeNotBuilt(const QueryEnvironment& env)
        {
            Emit(env, std::format("layer 3: not built -- build it with: {}", BuildDocsCommand(env)));
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

    namespace
    {
        // Absent (no error, nullopt) when Documents.db genuinely is not there or carries another
        // tool version's schema -- layer 3 is optional coverage, never a reason to fail q's or
        // status's layer-1/2 answer (decision 4). Unreadable (an error) when the path exists but
        // could not be opened -- corrupt, locked, permission denied -- which is a fact the
        // instrument could not establish and must never be folded into "not built" (finding 4).
        // Documents.cpp keeps its own OpenDocuments private, so this is its own small copy,
        // matching this codebase's existing preference for a local static over a shared header.
        struct DocumentsOrNone
        {
            std::optional<Store::Database> Database;
            std::optional<std::string>     UnreadableReason;
        };

        DocumentsOrNone OpenDocumentsOrNone(const QueryEnvironment& env)
        {
            if (env.Documents.empty())
            {
                return {};
            }
            std::error_code error;
            const bool      exists = std::filesystem::exists(env.Documents, error);
            if (error)
            {
                return {std::nullopt, error.message()};
            }
            if (!exists)
            {
                return {};
            }
            auto db = Store::Database::Open(env.Documents, Store::Database::Mode::ReadOnly);
            if (!db)
            {
                return {std::nullopt, db.error().Format()};
            }
            if (auto ok = Store::CheckDocumentsSchema(*db); !ok)
            {
                // CheckDocumentsSchema's own Mismatch means the file opened and reads fine, it
                // just carries a Kind/SchemaVersion this build does not expect -- decision 4's
                // "not built". Any other code (Database/Io) means the read that would answer
                // that question itself failed -- SQLite could not even get past the page header
                // (corrupt bytes, wrong format, a sharing violation) -- an honestly different
                // fact than "absent" (finding 4).
                if (ok.error().Code == Foundation::DiagnosticCode::Mismatch)
                {
                    return {};
                }
                return {std::nullopt, ok.error().Format()};
            }
            return {std::move(*db), std::nullopt};
        }

        std::int64_t ScalarOrZero(Store::Database& db, std::string_view sql)
        {
            auto value = db.ScalarInt(sql);
            return value ? *value : 0;
        }

        std::pair<std::uint64_t, std::uint64_t> DocumentsCoverage(Store::Database& db)
        {
            auto statement = db.Prepare("SELECT COALESCE(SUM(Read), 0), COALESCE(SUM(Total), 0) FROM Coverage");
            if (!statement)
            {
                return {0, 0};
            }
            const auto row = statement->Step();
            if (!row || !*row)
            {
                return {0, 0};
            }
            return {static_cast<std::uint64_t>(statement->Int(0)), static_cast<std::uint64_t>(statement->Int(1))};
        }

        void PrintCitedBy(const QueryEnvironment& env, std::string_view file, bool numberIsNull,
                          std::string_view number, std::string_view title)
        {
            if (numberIsNull || number.empty())
            {
                Emit(env, std::format("  cited by {} \xE2\x80\x94 {}", Basename(file), title));
            }
            else
            {
                Emit(env, std::format("  cited by {} \xC2\xA7{} \xE2\x80\x94 {}", Basename(file), number, title));
            }
        }

        void PrintSealedAt(const QueryEnvironment& env, std::string_view file, std::int64_t line, std::string_view tag,
                           bool imageIsNull, std::string_view image)
        {
            Emit(env, std::format("  sealed at {}:{} [{}] {}", file, line, tag,
                                  imageIsNull ? std::string_view("(no image)") : image));
        }

        // A prepare/bind failure here would otherwise make the whole "cited by"/"sealed at" line
        // never print, and a mid-loop Step() error reads as plain end-of-rows, silently stopping
        // the count short of the real total -- both are the instrument reporting a fact ("N")
        // where it cannot actually look. This always prints the line, and prints NOT VERIFIED
        // with the reason instead of a number when it cannot vouch for one (finding 3). Returns
        // the count it actually vouches for, nullopt on a NOT VERIFIED -- the caller's own signal
        // for whether a "0" here is safe to annotate with EmitAddressBlindSpot below.
        std::optional<std::size_t> EmitCountedRows(const QueryEnvironment& env, std::string_view label,
                            Foundation::Expected<Store::Statement> statement, std::uint64_t address,
                            const std::function<void(Store::Statement&)>& printRow)
        {
            if (!statement)
            {
                Emit(env, std::format("  {}: NOT VERIFIED -- {}", label, statement.error().Format()));
                return std::nullopt;
            }
            if (auto bind = statement->Bind(1, static_cast<std::int64_t>(address)); !bind)
            {
                Emit(env, std::format("  {}: NOT VERIFIED -- {}", label, bind.error().Format()));
                return std::nullopt;
            }
            std::size_t count = 0;
            for (;;)
            {
                const auto row = statement->Step();
                if (!row)
                {
                    Emit(env, std::format("  {}: NOT VERIFIED after {} row(s) -- {}", label, count, row.error().Format()));
                    return std::nullopt;
                }
                if (!*row)
                {
                    break;
                }
                ++count;
                if (env.Full || count <= 10)
                {
                    printRow(*statement);
                }
            }
            Emit(env, std::format("  {}: {}", label, count));
            return count;
        }

        // `table`'s own corpus-wide count of rows this layer indexed but whose Address column is
        // NULL -- an enumerable fact, not an estimate (Seal: SealExtractor.cpp's own "no-address"
        // shape, e.g. Source/DesignLibrary/LayerResolver.cpp:864; Citation: a section naming its
        // target by symbol only, CitationExtractor.cpp). A row of this shape is invisible to the
        // `WHERE Address = ?` query above, so its "0" answers only "no ROW carries THIS address",
        // never "nothing in the tree refers to it". Called only when that count came back 0; this
        // is that zero's own blind spot, stated instead of left silent (CLAUDE.md: "an instrument's
        // zero is not an absence").
        void EmitAddressBlindSpot(const QueryEnvironment& env, Store::Database& documents, std::string_view table,
                                  std::string_view label, std::string_view rowNoun)
        {
            auto blindSpot = documents.ScalarInt(std::format("SELECT COUNT(*) FROM {} WHERE Address IS NULL", table));
            if (!blindSpot)
            {
                Emit(env, std::format("  {} blind spot: NOT VERIFIED -- {}", label, blindSpot.error().Format()));
                return;
            }
            Emit(env, std::format("  {} blind spot: {} {} in the tree carry no address this layer can pair -- "
                                  "the 0 above means none of THOSE match, not that nothing refers to this address",
                                  label, *blindSpot, rowNoun));
        }
    }

    void PrintDocumentLayer(const QueryEnvironment& env, std::uint64_t address)
    {
        auto opened = OpenDocumentsOrNone(env);
        if (opened.UnreadableReason)
        {
            Emit(env, std::format("layer 3: NOT VERIFIED -- {}", *opened.UnreadableReason));
            return;
        }
        if (!opened.Database)
        {
            EmitLayerThreeNotBuilt(env);
            return;
        }
        auto& documents = *opened.Database;
        const auto citedCount = EmitCountedRows(env, "cited by",
                       documents.Prepare("SELECT Section.File, Section.Number, Section.Title FROM Citation "
                                          "JOIN Section ON Section.Id = Citation.Section WHERE Citation.Address = ?1 "
                                          "ORDER BY Section.File, Section.FirstLine"),
                       address, [&env](Store::Statement& s) {
                           const bool numberIsNull = s.IsNull(1);
                           PrintCitedBy(env, s.Text(0), numberIsNull, numberIsNull ? std::string_view{} : s.Text(1),
                                       s.Text(2));
                       });
        if (citedCount && *citedCount == 0)
        {
            EmitAddressBlindSpot(env, documents, "Citation", "cited by", "citation(s)");
        }
        const auto sealedCount = EmitCountedRows(env, "sealed at",
                       documents.Prepare("SELECT File, Line, Tag, Image FROM Seal WHERE Address = ?1 ORDER BY File, Line"),
                       address, [&env](Store::Statement& s) {
                           const bool imageIsNull = s.IsNull(3);
                           PrintSealedAt(env, s.Text(0), s.Int(1), s.Text(2), imageIsNull,
                                        imageIsNull ? std::string_view{} : s.Text(3));
                       });
        if (sealedCount && *sealedCount == 0)
        {
            EmitAddressBlindSpot(env, documents, "Seal", "sealed at", "seal(s)");
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

        // Layer 3 answers "what does the corpus's prose say about this address" -- independent
        // of whether layer 1/2 ever built the owning image or resolved a function at it, so it
        // runs here, as soon as the address itself is concrete and mapped (decision 4).
        PrintDocumentLayer(env, *address);

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

    Verdict RunVirtualCall(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t site)
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
        const auto owned = Resolve(cache, *rows, site);
        if (!owned)
        {
            if (!cache.IsMapped(site))
            {
                return {VerdictKind::NotVerified, 0, std::format("0x{:x} is not mapped", site), 0, 0};
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

        auto statement = db->Prepare("SELECT Candidate, CandidateSymbol, SlotOffset, Discriminator, Instruction "
                                     "FROM VirtualCall WHERE Site = ?1 ORDER BY Candidate");
        if (!statement)
        {
            return {VerdictKind::NotVerified, 0, statement.error().Format(), owned->Image.CoverageRead,
                    owned->Image.CoverageTotal};
        }
        if (auto bind = statement->Bind(1, static_cast<std::int64_t>(site)); !bind)
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
            if (count == 0)
            {
                Emit(env, std::format("  slot=0x{:x}  D=0x{:x}  {}", statement->Int(2), statement->Int(3),
                                      statement->Text(4)));
            }
            ++count;
            if (env.Full || count <= 10)
            {
                Emit(env, std::format("    0x{:x}  {}", statement->Int(0), statement->Text(1)));
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

    Verdict RunFunction(const QueryEnvironment& env, const DyldSharedCache::Cache* cache,
                        std::uint64_t address, bool disassemble)
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

            // The function that CONTAINS the address, not the one that starts at it: a laudo cites
            // the address of a field or a call site far more often than a function's first byte.
            auto enclosing = db->Prepare("SELECT Address, Size FROM Function WHERE Address <= ?1 "
                                         "ORDER BY Address DESC LIMIT 1");
            if (!enclosing)
            {
                return {VerdictKind::NotVerified, 0, enclosing.error().Format(), read, total};
            }
            if (auto bind = enclosing->Bind(1, static_cast<std::int64_t>(address)); !bind)
            {
                return {VerdictKind::NotVerified, 0, bind.error().Format(), read, total};
            }
            const auto row = enclosing->Step();
            if (!row)
            {
                return {VerdictKind::NotVerified, 0, row.error().Format(), read, total};
            }
            if (!*row)
            {
                continue;
            }
            const auto start = static_cast<std::uint64_t>(enclosing->Int(0));
            const auto size = static_cast<std::uint64_t>(enclosing->Int(1));
            if (size == 0 || address >= start + size)
            {
                continue;
            }

            const std::string basename = std::filesystem::path(image.Path).filename().string();
            Emit(env, std::format("0x{:x} is in 0x{:x} (+0x{:x}), {} bytes, in {}", address, start,
                                  address - start, size, basename));

            if (disassemble)
            {
                if (cache == nullptr)
                {
                    return {VerdictKind::NotVerified, 0,
                            "--asm reads the cache; pass --cache or set SHERLOCK_CACHE", read, total};
                }
                auto bytes = cache->Read(start, static_cast<std::size_t>(size));
                if (!bytes)
                {
                    return {VerdictKind::NotVerified, 0, bytes.error().Format(), read, total};
                }
                auto disassembler = Facts::Disassembler::Create();
                if (!disassembler)
                {
                    return {VerdictKind::NotVerified, 0, disassembler.error().Format(), read, total};
                }
                std::size_t printed = 0;
                auto coverage = disassembler->Stream(*bytes, start, [&](const Facts::Instruction& one) {
                    Emit(env, std::format("  0x{:x}  {} {}", one.Address, one.Mnemonic, one.Operands));
                    ++printed;
                });
                if (!coverage)
                {
                    return {VerdictKind::NotVerified, 0, coverage.error().Format(), read, total};
                }
                Emit(env, std::format("layer 1: {}/{} instruction words decoded", coverage->Decoded,
                                      coverage->Total));
                return {VerdictKind::Found, printed, {}, coverage->Decoded, coverage->Total};
            }

            // basename, not image.Name: the latter is the layer-1 store's file name.
            const auto storePath = HexRaysExport::StorePath(env.Store / "Images", basename);
            std::error_code exists;
            if (!std::filesystem::is_regular_file(storePath, exists))
            {
                // The zero rule: layer 2 has nothing here because it was never built for this
                // image, and the line says which command builds it rather than reporting absence.
                Emit(env, std::format("layer 2: not built for {} -- build it with: Sherlock build "
                                      "hexrays --store {} --images {}",
                                      basename, env.Store.string(), basename));
                return {VerdictKind::Empty, 0, "layer 2 is not built for this image", read, total};
            }
            auto layerTwo = Store::Database::Open(storePath, Store::Database::Mode::ReadOnly);
            if (!layerTwo)
            {
                return {VerdictKind::NotVerified, 0, layerTwo.error().Format(), read, total};
            }
            if (auto ok = HexRaysExport::CheckStoreSchema(*layerTwo); !ok)
            {
                return {VerdictKind::NotVerified, 0, ok.error().Format(), read, total};
            }
            auto coverage = HexRaysExport::ReadCoverage(*layerTwo);
            const std::uint64_t decompiled = coverage ? coverage->Decompiled : 0;
            const std::uint64_t attempted = coverage ? coverage->Attempted : 0;

            auto stored = HexRaysExport::ReadFunction(*layerTwo, start);
            if (!stored)
            {
                return {VerdictKind::NotVerified, 0, stored.error().Format(), decompiled, attempted};
            }
            if (!*stored)
            {
                Emit(env, std::format("layer 2: no row for 0x{:x}", start));
                return {VerdictKind::Empty, 0, "this function is not in layer 2", decompiled, attempted};
            }
            if ((*stored)->State != HexRaysExport::Status::Ok)
            {
                Emit(env, std::format("layer 2: {} -- {}",
                                      HexRaysExport::ToText((*stored)->State), (*stored)->Reason));
                return {VerdictKind::Empty, 0, "this function did not decompile", decompiled, attempted};
            }

            // Pseudocode locates; on its own it never closes a decoded value (spec section 6).
            Emit(env, std::format("layer 2: {} line(s), {:.3f} s, pseudocode locates and does not "
                                  "close a value", (*stored)->Lines, (*stored)->Seconds));

            // The carved slice loses every target outside the image, and layer 2 prints those
            // calls as MEMORY[<island>]. Layer 1 read the WHOLE cache and stored the island
            // beside the target it jumps to, so the lost name is one join away -- performed here
            // rather than named as a command for the reader to run, which is what it was.
            const auto islands = ResolveIslands(env, *db, *rows, *catalogUuid, start, start + size);
            std::map<std::uint64_t, ResolvedIsland> resolved;
            if (islands)
            {
                resolved = *islands;
            }
            std::size_t named = 0;
            std::vector<std::uint64_t> unresolved;
            std::set<std::string> remaining;
            for (const auto& [island, entry] : resolved)
            {
                if (entry.Symbol.empty())
                {
                    unresolved.push_back(island);
                }
                else
                {
                    ++named;
                }
            }
            if (!resolved.empty())
            {
                Emit(env, std::format("layer 2: {} of {} call island(s) leaving {} named from "
                                      "layer 1", named, resolved.size(), basename));
            }
            // The zero rule on the substitution itself, and the two reasons kept apart because
            // they have different answers: a target outside the indexed towers is the store's
            // scope (the cache has thousands of images and this store holds the towers), while a
            // target inside one with no symbol at it is that image's symbol table.
            std::string outside, unnamed;
            for (const auto island : unresolved)
            {
                const auto& entry = resolved[island];
                auto& into = entry.Image.empty() ? outside : unnamed;
                into += std::format("{}0x{:x}", into.empty() ? "" : " ", entry.Target);
            }
            if (!outside.empty())
            {
                Emit(env, std::format("layer 2: target(s) outside the {} indexed image(s), so "
                                      "their call keeps its address: {}", rows->size(), outside));
            }
            if (!unnamed.empty())
            {
                Emit(env, std::format("layer 2: target(s) inside an indexed image that carries no "
                                      "symbol at them: {}", unnamed));
            }
            for (const auto& line : SplitLines((*stored)->Pseudocode))
            {
                std::string text(line);
                for (const auto& [island, entry] : resolved)
                {
                    if (entry.Symbol.empty())
                    {
                        continue;
                    }
                    const auto placeholder = std::format("MEMORY[0x{:X}]", island);
                    for (auto at = text.find(placeholder); at != std::string::npos;
                         at = text.find(placeholder, at + entry.Symbol.size()))
                    {
                        text.replace(at, placeholder.size(), entry.Symbol);
                    }
                }
                // Counted from the text that is actually printed, not from what the join
                // covered: a placeholder the reader can still see is a hole whatever the
                // resolution rate above says, and the two are different sets.
                for (auto at = text.find("MEMORY[0x"); at != std::string::npos;
                     at = text.find("MEMORY[0x", at + 1))
                {
                    const auto close = text.find(']', at);
                    if (close == std::string::npos)
                    {
                        continue;
                    }
                    const auto printed = text.substr(at + 7, close - at - 7);
                    // An island layer 1 knows but could not name is already accounted for by the
                    // two lines above; counting it here too would report one hole as two.
                    bool known = false;
                    for (const auto& [island, entry] : resolved)
                    {
                        (void)entry;
                        if (std::format("0x{:X}", island) == printed)
                        {
                            known = true;
                            break;
                        }
                    }
                    if (!known)
                    {
                        remaining.insert(printed);
                    }
                }
                Emit(env, text);
            }
            // The bound this feature can never cross, stated where it bites: the store indexes
            // the towers, not the cache's four thousand images, so a call into anything else --
            // the Swift runtime above all -- has no row to join against and keeps its address.
            if (!remaining.empty())
            {
                std::string list;
                for (const auto& address : remaining)
                {
                    list += (list.empty() ? "" : " ") + address;
                }
                Emit(env, std::format("layer 2: {} address(es) still print as MEMORY[...]: {} -- "
                                      "they are not call islands layer 1 recorded, and fall in no "
                                      "segment of the {} indexed image(s)",
                                      remaining.size(), list, rows->size()));
            }
            // The mangled symbol goes inline because it is one token and keeps the pseudocode's
            // shape; the human-readable form is longer than a line of C, so --full is where it
            // lands, with the island and the target beside it for a seal to cite.
            if (env.Full)
            {
                for (const auto& [island, entry] : resolved)
                {
                    Emit(env, std::format("  0x{:x} -> 0x{:x}  {}  {}", island, entry.Target,
                                          entry.Image.empty() ? "?" : entry.Image,
                                          entry.Demangled.empty() ? entry.Symbol : entry.Demangled));
                }
            }
            return {VerdictKind::Found, (*stored)->Lines, {}, decompiled, attempted};
        }

        Emit(env, std::format("layer 1: no function contains 0x{:x}", address));
        return {VerdictKind::Empty, 0, "no function contains this address", read, total};
    }

    Verdict RunGrep(const QueryEnvironment& env, std::string_view pattern,
                    const std::vector<std::string>& images, bool ignoreCase, std::size_t limit)
    {
        if (pattern.empty())
        {
            return {VerdictKind::NotVerified, 0, "no pattern given", 0, 0};
        }

        auto catalog = Store::Database::Open(env.Store / "Catalog.db", Store::Database::Mode::ReadOnly);
        if (!catalog)
        {
            return {VerdictKind::NotVerified, 0, catalog.error().Format(), 0, 0};
        }
        const auto catalogUuid = ValidateCatalog(*catalog);
        if (!catalogUuid)
        {
            return {VerdictKind::NotVerified, 0, catalogUuid.error().Format(), 0, 0};
        }
        const auto rows = LoadImages(*catalog);
        if (!rows)
        {
            return {VerdictKind::NotVerified, 0, rows.error().Format(), 0, 0};
        }

        const std::string needle = ignoreCase ? Lowered(pattern) : std::string(pattern);
        std::size_t hits = 0, searchedImages = 0, selectedImages = 0;
        std::uint64_t searchedFunctions = 0, refused = 0, crossImageHits = 0, unindexed = 0;
        std::vector<std::string> withoutLayerTwo, partial;
        bool truncated = false;

        // The selected set is built before the walk, so that a limit which stops the walk early
        // cannot also shorten the list of what went unexamined. An image the search never reached
        // is a gap exactly like an image with no layer 2, and the two are reported side by side.
        std::vector<std::string> selected;
        for (const auto& image : *rows)
        {
            const auto basename = std::filesystem::path(image.Path).filename().string();
            if (images.empty() ||
                std::find(images.begin(), images.end(), basename) != images.end())
            {
                selected.push_back(basename);
            }
        }
        selectedImages = selected.size();

        // A NAME THAT MATCHES NO IMAGE IS AN ERROR, NOT AN EMPTY SEARCH. Without
        // this the misspelling searches nothing and reports `verdict: EMPTY`,
        // which reads as "the pattern is not in the corpus" -- the exact
        // confusion the rest of this command's reporting exists to prevent.
        // Measured: `--images AppKit,DesignLibrary` selected zero images and
        // said EMPTY with `coverage 0/0`.
        for (const auto& wanted : images)
        {
            if (std::find(selected.begin(), selected.end(), wanted) != selected.end()) continue;

            std::string known;
            for (const auto& image : *rows)
            {
                if (!known.empty()) known += ", ";
                known += std::filesystem::path(image.Path).filename().string();
            }
            return {VerdictKind::NotVerified, 0,
                    std::format("--images names {} and the catalog has no such image; it holds {}",
                                wanted, known),
                    0, 0};
        }

        std::vector<std::string> unreached;
        for (std::size_t index = 0; index < selected.size(); ++index)
        {
            const auto& basename = selected[index];
            if (truncated)
            {
                unreached.push_back(basename);
                continue;
            }

            const auto storePath = HexRaysExport::StorePath(env.Store / "Images", basename);
            std::error_code exists;
            if (!std::filesystem::is_regular_file(storePath, exists))
            {
                withoutLayerTwo.push_back(basename);
                continue;
            }
            auto layerTwo = Store::Database::Open(storePath, Store::Database::Mode::ReadOnly);
            if (!layerTwo || !HexRaysExport::CheckStoreSchema(*layerTwo))
            {
                withoutLayerTwo.push_back(basename);
                continue;
            }

            // A partial store is searchable and incomplete at the same time, which is the worst
            // shape for a zero: it answers for the functions it holds and is silent about the
            // rest. Named here rather than treated as whole.
            if (auto whole = HexRaysExport::IsComplete(*layerTwo); whole && !*whole)
            {
                auto held = HexRaysExport::ReadCoverage(*layerTwo);
                partial.push_back(std::format("{} ({} function(s) so far)", basename,
                                              held ? held->Attempted : 0));
            }
            if (auto missing = HexRaysExport::CountWithoutPseudocode(*layerTwo))
            {
                refused += *missing;
            }

            ++searchedImages;

            // Which of this image's islands carry a symbol the pattern matches. Only those are
            // needed: a line mentioning one of them is a hit even though the stored text has the
            // island's ADDRESS where its name would be, and substituting every island in every
            // line to discover that would be the same answer for far more work.
            std::map<std::uint64_t, std::string> crossing;
            const auto row = std::find_if(rows->begin(), rows->end(), [&](const ImageRow& candidate) {
                return std::filesystem::path(candidate.Path).filename().string() == basename;
            });
            if (auto db = row == rows->end()
                              ? Foundation::Expected<Store::Database>(std::unexpected(
                                    Foundation::Fail(Foundation::DiagnosticCode::NotFound,
                                                     Foundation::Severity::NotVerified,
                                                     "Cli::RunGrep", basename,
                                                     "no catalog row for this image",
                                                     "rebuild the catalog").error()))
                              : OpenImage(env, *row, *catalogUuid))
            {
                if (auto all = IslandSymbols(env, *rows, *catalogUuid, *db))
                {
                    for (const auto& [island, symbol] : *all)
                    {
                        const bool hit = ignoreCase ? Lowered(symbol).find(needle) != std::string::npos
                                                    : symbol.find(needle) != std::string::npos;
                        if (hit)
                        {
                            crossing[island] = symbol;
                        }
                    }
                }
                else
                {
                    ++unindexed;
                }
            }
            else
            {
                ++unindexed;
            }

            const auto walked = HexRaysExport::ForEachPseudocode(
                *layerTwo, [&](std::uint64_t address, std::string_view text) {
                    ++searchedFunctions;
                    std::size_t number = 0;
                    for (const auto& line : SplitLines(text))
                    {
                        ++number;
                        bool found = ignoreCase ? Lowered(line).find(needle) != std::string::npos
                                                : line.find(needle) != std::string_view::npos;
                        std::string shown;
                        if (!found)
                        {
                            for (const auto& [island, symbol] : crossing)
                            {
                                const auto placeholder = std::format("MEMORY[0x{:X}]", island);
                                if (line.find(placeholder) == std::string_view::npos)
                                {
                                    continue;
                                }
                                found = true;
                                shown = std::string(line);
                                for (auto at = shown.find(placeholder); at != std::string::npos;
                                     at = shown.find(placeholder, at + symbol.size()))
                                {
                                    shown.replace(at, placeholder.size(), symbol);
                                }
                                ++crossImageHits;
                                break;
                            }
                        }
                        if (!found)
                        {
                            continue;
                        }
                        ++hits;
                        if (limit > 0 && hits > limit)
                        {
                            truncated = true;
                            return false;
                        }
                        Emit(env, std::format("{} 0x{:x}:{}: {}", basename, address, number,
                                              shown.empty() ? std::string(line) : shown));
                    }
                    return true;
                });
            if (!walked)
            {
                return {VerdictKind::NotVerified, 0, walked.error().Format(),
                        searchedImages, selectedImages};
            }
        }

        if (truncated)
        {
            --hits;
            Emit(env, std::format("stopped at {} hit(s); the rest is not searched -- raise --limit",
                                  limit));
        }
        // The zero rule, and this command's whole reason for stating it: an image with no layer 2
        // is silent in exactly the way an image without the pattern is, so a reader who is not
        // told cannot tell "not there" from "never looked".
        if (!withoutLayerTwo.empty())
        {
            std::string names;
            for (const auto& name : withoutLayerTwo)
            {
                names += (names.empty() ? "" : " ") + name;
            }
            Emit(env, std::format("not searched: {} -- no layer 2 there; build it with: Sherlock "
                                  "build hexrays --store {} --images {}",
                                  names, env.Store.string(), names));
        }
        if (!unreached.empty())
        {
            std::string names;
            for (const auto& name : unreached)
            {
                names += (names.empty() ? "" : " ") + name;
            }
            Emit(env, std::format("not reached: {} -- the limit stopped the search before these, "
                                  "and they are not evidence of anything", names));
        }
        for (const auto& image : partial)
        {
            Emit(env, std::format("partial: {} -- an interrupted export; what it has not reached "
                                  "is not searched", image));
        }
        if (refused > 0)
        {
            Emit(env, std::format("{} function(s) in the searched images carry no pseudocode -- "
                                  "the decompiler refused them, and no text search reaches inside",
                                  refused));
        }
        // The blind spot that makes this command's zero dangerous, printed on every search
        // rather than only on an empty one. Layer 2 decompiles a slice carved out of the cache,
        // so a call leaving the image reads as MEMORY[0x...] with no symbol on it; a target in
        // one of the indexed images is matched by the symbol layer 1 holds for it, and a target
        // anywhere else in the cache carries no name for any pattern to match. The two counts
        // are separate because they send a reader at different instruments.
        Emit(env, std::format("searched {} function(s) in {} image(s); {} hit(s) matched a name "
                              "the carved slice does not carry, resolved through layer 1's call "
                              "islands", searchedFunctions, searchedImages, crossImageHits));
        if (unindexed > 0)
        {
            Emit(env, std::format("{} image(s) had no layer-1 store to resolve their islands "
                                  "against, so a cross-image name in them stays unfindable",
                                  unindexed));
        }
        Emit(env, std::format("a call into anything outside the {} indexed image(s) -- the Swift "
                              "and ObjC runtimes above all -- still has no name to match",
                              rows->size()));

        if (hits == 0)
        {
            return {VerdictKind::Empty, 0, {}, searchedImages, selectedImages};
        }
        return {VerdictKind::Found, hits, {}, searchedImages, selectedImages};
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

        auto opened = OpenDocumentsOrNone(env);
        if (opened.UnreadableReason)
        {
            Emit(env, std::format("layer 3: NOT VERIFIED -- {}", *opened.UnreadableReason));
        }
        else if (!opened.Database)
        {
            EmitLayerThreeNotBuilt(env);
        }
        else
        {
            auto&      documents          = *opened.Database;
            const auto sections            = ScalarOrZero(documents, "SELECT COUNT(*) FROM Section");
            const auto citations           = ScalarOrZero(documents, "SELECT COUNT(*) FROM Citation");
            const auto seals               = ScalarOrZero(documents, "SELECT COUNT(*) FROM Seal");
            const auto [docRead, docTotal] = DocumentsCoverage(documents);
            Emit(env, std::format("layer 3: {} section(s), {} citation(s), {} seal(s), coverage {}/{} file(s)",
                                  sections, citations, seals, docRead, docTotal));
            if (auto head = Store::ReadMeta(documents, "Head"))
            {
                Emit(env, std::format("head: {} (information only -- the check below is per file, not per commit)",
                                      *head));
            }

            // The one place this layer pays the full cost: a stat() per indexed file (cheap,
            // no content read) plus a walk for files the store has no row for at all -- this is
            // what catches an uncommitted in-place edit, which changes neither HEAD nor the total
            // file count (decision 8). The walk itself reuses BuildDocuments's own inclusion
            // rule (DocumentIndex::IsIndexedMarkdown, DocumentIndex::WalkSourceForTesting) instead
            // of a second, hand-rolled one -- two rules for "is this file in the corpus" drift
            // apart the day one of them changes, and a stale README.md/index.md exclusion is
            // exactly how "added" ends up permanently nonzero (finding 1). A failure here (a
            // SQLite error mid-scan, or a directory walk the filesystem refuses) is a fact the
            // instrument could not establish and must surface as NOT VERIFIED, never as a
            // reassuring zero (finding 2).
            std::size_t                        changed = 0, removed = 0, added = 0;
            std::set<std::string, std::less<>> known;
            std::optional<std::string>         scanError;
            auto files = documents.Prepare("SELECT Path, Size, MTime FROM File");
            if (!files)
            {
                scanError = files.error().Format();
            }
            else
            {
                for (;;)
                {
                    const auto row = files->Step();
                    if (!row)
                    {
                        scanError = row.error().Format();
                        break;
                    }
                    if (!*row) break;
                    const std::string path(files->Text(0));
                    known.insert(path);
                    const auto current = DocumentIndex::StatFile(env.Repo / path);
                    if (!current)
                    {
                        ++removed;
                    }
                    else if (current->Size != static_cast<std::uintmax_t>(files->Int(1)) ||
                            current->MTime != files->Int(2))
                    {
                        ++changed;
                    }
                }
            }
            const auto countNew = [&](Foundation::Expected<std::vector<std::filesystem::path>> found) {
                if (scanError) return;
                if (!found)
                {
                    scanError = found.error().Format();
                    return;
                }
                for (const auto& file : *found)
                {
                    const auto rel = std::filesystem::relative(file, env.Repo).generic_string();
                    if (!known.contains(rel))
                    {
                        ++added;
                    }
                }
            };
            if (!scanError) countNew(DocumentIndex::WalkMarkdownForCli(env.Repo / "docs" / "re"));
            if (!scanError) countNew(DocumentIndex::WalkMarkdownForCli(env.Repo / "docs" / "concepts"));
            if (!scanError) countNew(DocumentIndex::WalkSourceForTesting(env.Repo / "Source", {}));

            if (scanError)
            {
                return {VerdictKind::NotVerified, done,
                        std::format("layer 3 staleness scan could not finish: {}", *scanError), read, total};
            }
            Emit(env, std::format("changed: {}, added: {}, removed: {} (since this store was built)", changed, added,
                                  removed));
        }

        return {VerdictKind::Found, done, {}, read, total};
    }
}
