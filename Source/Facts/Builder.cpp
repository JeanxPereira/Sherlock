// Sherlock — tools/Sherlock/Source/Facts/Builder.cpp
// Catalog and per-image stores, filled by worker threads that each own a Disassembler and a Database.
#include <Facts/Builder.h>

#include <Facts/Disassembler.h>
#include <Facts/ImageFacts.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace Sherlock::Facts
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        struct WorkItem
        {
            std::string           Path;
            std::string           Tower;
            std::uint64_t         Header = 0;
            std::filesystem::path StorePath;
            std::string           Basename;
        };

        struct WorkResult
        {
            WorkItem      Item;
            bool          Ok = false;
            std::string   Reason;
            std::uint64_t Decoded = 0;
            std::uint64_t Total   = 0;
        };

        std::string BasenameOf(const std::string& path)
        {
            const auto slash = path.rfind('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }
    }

    Expected<BuildReport> BuildFacts(const DyldSharedCache::Cache& cache, std::string_view build,
                                     const BuildOptions& options)
    {
        std::error_code ignored;
        std::filesystem::create_directories(options.Store / "Images", ignored);

        const auto catalogPath = options.Store / "Catalog.db";
        auto       catalog     = Store::Database::Open(catalogPath, Store::Database::Mode::ReadWrite);
        if (!catalog)
        {
            return std::unexpected(catalog.error());
        }
        const auto tableCount = catalog->ScalarInt("SELECT count(*) FROM sqlite_master WHERE name = 'Meta'");
        if (!tableCount)
        {
            return std::unexpected(tableCount.error());
        }
        if (*tableCount == 0)
        {
            if (auto ok = Store::CreateCatalog(*catalog, build, cache.Uuid()); !ok)
            {
                return std::unexpected(ok.error());
            }
        }
        else
        {
            if (auto ok = Store::CheckSchema(*catalog, "Catalog"); !ok)
            {
                return std::unexpected(ok.error());
            }
            auto uuidQuery = catalog->Prepare("SELECT Value FROM Meta WHERE Key = 'CacheUuid'");
            if (!uuidQuery)
            {
                return std::unexpected(uuidQuery.error());
            }
            const auto row = uuidQuery->Step();
            if (!row)
            {
                return std::unexpected(row.error());
            }
            if (!*row || uuidQuery->Text(0) != cache.Uuid())
            {
                return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "BuildFacts", catalogPath.string(),
                            "the catalog's CacheUuid does not match this cache",
                            "point --store at this build's own directory");
            }
        }

        std::unordered_map<std::string, int> seen;
        std::vector<WorkItem>                items;
        for (const auto& [path, tower] : options.Images)
        {
            const DyldSharedCache::CacheImage* found = nullptr;
            for (const auto& candidate : cache.Images())
            {
                if (candidate.Path == path)
                {
                    found = &candidate;
                    break;
                }
            }
            if (found == nullptr)
            {
                continue;
            }
            if (options.Resume)
            {
                auto state = catalog->Prepare("SELECT State FROM Image WHERE Path = ?1");
                if (state && state->Bind(1, path))
                {
                    const auto has = state->Step();
                    if (has && *has && state->Text(0) == "FactsDone")
                    {
                        continue;
                    }
                }
            }
            const std::string base  = BasenameOf(path);
            const int         count = ++seen[base];
            const std::string file  = count == 1 ? base + ".db" : base + "-" + std::to_string(count) + ".db";
            // Image.Name carries the store FILE name (with its collision suffix), not the bare basename --
            // a later process (a query, run cold) reconstructs StorePath from Name alone, never recomputing
            // which images collided.
            items.push_back({path, tower, found->Header, options.Store / "Images" / file, file});
        }

        std::queue<WorkItem> pending;
        for (auto& item : items)
        {
            pending.push(item);
        }
        std::mutex              queueMutex;
        std::mutex              resultMutex;
        std::vector<WorkResult> results;

        const unsigned workerCount =
            options.Workers != 0 ? options.Workers : std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> workers;
        for (unsigned w = 0; w < workerCount; ++w)
        {
            workers.emplace_back([&]() {
                for (;;)
                {
                    WorkItem item;
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        if (pending.empty())
                        {
                            return;
                        }
                        item = pending.front();
                        pending.pop();
                    }

                    WorkResult result;
                    result.Item = item;

                    const auto fail = [&](std::string reason) {
                        result.Reason = std::move(reason);
                        std::lock_guard<std::mutex> lock(resultMutex);
                        results.push_back(std::move(result));
                    };

                    auto disassembler = Disassembler::Create();
                    if (!disassembler)
                    {
                        fail(disassembler.error().Format());
                        continue;
                    }
                    auto facts = ExtractImage(cache, DyldSharedCache::CacheImage{item.Path, item.Header}, *disassembler);
                    if (!facts)
                    {
                        fail(facts.error().Format());
                        continue;
                    }
                    auto db = Store::Database::Open(item.StorePath, Store::Database::Mode::ReadWrite);
                    if (!db)
                    {
                        fail(db.error().Format());
                        continue;
                    }
                    if (auto ok = Store::CreateImageStore(*db, item.Path); !ok)
                    {
                        fail(ok.error().Format());
                        continue;
                    }
                    if (auto ok = WriteImageFacts(*db, *facts, options.Demangle); !ok)
                    {
                        fail(ok.error().Format());
                        continue;
                    }

                    result.Ok = true;
                    // Facts::StreamCoverage already counts instruction words (Stream divides by 4 as it
                    // scans), matching litref.py's own unit -- no second division belongs here.
                    result.Decoded = facts->Coverage.Decoded;
                    result.Total   = facts->Coverage.Total;
                    std::printf("%s  FactsDone  coverage %llu/%llu\n", item.Path.c_str(),
                               static_cast<unsigned long long>(result.Decoded),
                               static_cast<unsigned long long>(result.Total));
                    std::lock_guard<std::mutex> lock(resultMutex);
                    results.push_back(std::move(result));
                }
            });
        }
        for (auto& worker : workers)
        {
            worker.join();
        }

        auto upsertImage = catalog->Prepare("INSERT OR REPLACE INTO Image(Path, Name, Tower, Header, State, Reason) "
                                            "VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
        auto upsertCoverage = catalog->Prepare(
            "INSERT OR REPLACE INTO Coverage(Image, Layer, Unit, Read, Total) VALUES(?1, 'Facts', 'instructions', ?2, ?3)");
        if (!upsertImage || !upsertCoverage)
        {
            return std::unexpected((!upsertImage ? upsertImage : upsertCoverage).error());
        }

        BuildReport report;
        for (const auto& result : results)
        {
            if (auto ok = upsertImage->Bind(1, result.Item.Path); !ok) return std::unexpected(ok.error());
            if (auto ok = upsertImage->Bind(2, result.Item.Basename); !ok) return std::unexpected(ok.error());
            if (result.Item.Tower.empty())
            {
                if (auto ok = upsertImage->BindNull(3); !ok) return std::unexpected(ok.error());
            }
            else if (auto ok = upsertImage->Bind(3, result.Item.Tower); !ok)
            {
                return std::unexpected(ok.error());
            }
            if (auto ok = upsertImage->Bind(4, static_cast<std::int64_t>(result.Item.Header)); !ok)
            {
                return std::unexpected(ok.error());
            }
            if (auto ok = upsertImage->Bind(5, std::string_view(result.Ok ? "FactsDone" : "FactsFailed")); !ok)
            {
                return std::unexpected(ok.error());
            }
            if (result.Ok)
            {
                if (auto ok = upsertImage->BindNull(6); !ok) return std::unexpected(ok.error());
            }
            else if (auto ok = upsertImage->Bind(6, result.Reason); !ok)
            {
                return std::unexpected(ok.error());
            }
            if (auto ok = upsertImage->Step(); !ok) return std::unexpected(ok.error());
            if (auto ok = upsertImage->Reset(); !ok) return std::unexpected(ok.error());

            if (result.Ok)
            {
                if (auto ok = upsertCoverage->Bind(1, result.Item.Path); !ok) return std::unexpected(ok.error());
                if (auto ok = upsertCoverage->Bind(2, static_cast<std::int64_t>(result.Decoded)); !ok)
                {
                    return std::unexpected(ok.error());
                }
                if (auto ok = upsertCoverage->Bind(3, static_cast<std::int64_t>(result.Total)); !ok)
                {
                    return std::unexpected(ok.error());
                }
                if (auto ok = upsertCoverage->Step(); !ok) return std::unexpected(ok.error());
                if (auto ok = upsertCoverage->Reset(); !ok) return std::unexpected(ok.error());
                ++report.Done;
            }
            else
            {
                report.Failed.push_back({result.Item.Path, result.Reason});
            }
        }
        return report;
    }
}
