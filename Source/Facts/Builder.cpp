// Sherlock — tools/Sherlock/Source/Facts/Builder.cpp
// Catalog and per-image stores, filled by worker threads that each own a Disassembler and a Database.
#include <Facts/Builder.h>

#include <Facts/Disassembler.h>
#include <Facts/ImageFacts.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Sherlock::Facts
{
    using Foundation::Diagnostic;
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        struct WorkItem
        {
            std::string Path;
            std::string Tower;
            std::uint64_t Header = 0;
            std::filesystem::path StorePath;
            std::string Basename;
            bool HadUsableStore = false;
        };

        struct WorkResult
        {
            WorkItem Item;
            bool Ok = false;
            std::optional<Diagnostic> Error;
            std::uint64_t Decoded = 0;
            std::uint64_t Total = 0;
            std::uintmax_t Bytes = 0;
            double Seconds = 0;
            double PeakMiB = 0;
            Diagnostic Emergency;
        };

        std::string BasenameOf(const std::string& path)
        {
            const auto slash = path.rfind('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        Expected<bool> ValidImageStore(const std::filesystem::path& path, std::string_view image,
                                       std::string_view cacheUuid)
        {
            if (!std::filesystem::is_regular_file(path)) return false;
            auto db = Store::Database::Open(path, Store::Database::Mode::ReadOnly);
            if (!db) return false;
            if (auto ok = Store::CheckSchema(*db, "Image"); !ok) return false;
            const auto actualImage = Store::ReadMeta(*db, "ImagePath");
            const auto actualUuid = Store::ReadMeta(*db, "CacheUuid");
            return actualImage && actualUuid && *actualImage == image && *actualUuid == cacheUuid;
        }

        Expected<void> ReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination)
        {
            if (!::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildFacts::ReplaceFile", destination.string(),
                            "the completed image store could not replace the prior store", "check the corpus disk",
                            Foundation::LastSystemError());
            }
            return {};
        }
    }

    Expected<bool> HasCompletedFacts(Store::Database& catalog, std::string_view imagePath)
    {
        auto statement = catalog.Prepare(
            "SELECT I.State, C.Unit, C.Read, C.Total FROM Image I "
            "LEFT JOIN Coverage C ON C.Image = I.Path AND C.Layer = 'Facts' WHERE I.Path = ?1");
        if (!statement) return std::unexpected(statement.error());
        if (auto ok = statement->Bind(1, imagePath); !ok) return std::unexpected(ok.error());
        const auto row = statement->Step();
        if (!row) return std::unexpected(row.error());
        if (!*row || statement->Text(0) != "FactsDone" || statement->Text(1) != "instructions") return false;
        const auto read = statement->Int(2);
        const auto total = statement->Int(3);
        return read >= 0 && total > 0 && read <= total;
    }

    Expected<BuildReport> BuildFacts(const DyldSharedCache::Cache& cache, std::string_view build,
                                     const BuildOptions& options)
    {
        if (options.Store.empty())
        {
            return Fail(DiagnosticCode::Usage, Severity::NotVerified, "BuildFacts", "--store",
                        "no store directory was provided", "pass --store or set SHERLOCK_STORE");
        }
        std::error_code fsError;
        std::filesystem::create_directories(options.Store / "Images", fsError);
        if (fsError)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildFacts", options.Store.string(),
                        "the store directory cannot be created", "check the corpus disk", fsError.message());
        }

        const auto catalogPath = options.Store / "Catalog.db";
        auto catalog = Store::Database::Open(catalogPath, Store::Database::Mode::ReadWrite);
        if (!catalog) return std::unexpected(catalog.error());
        const auto tableCount = catalog->ScalarInt("SELECT count(*) FROM sqlite_master WHERE name = 'Meta'");
        if (!tableCount) return std::unexpected(tableCount.error());
        if (*tableCount == 0)
        {
            if (auto ok = Store::CreateCatalog(*catalog, build, cache.Uuid()); !ok) return std::unexpected(ok.error());
        }
        else
        {
            if (auto ok = Store::CheckSchema(*catalog, "Catalog"); !ok) return std::unexpected(ok.error());
            const auto uuid = Store::ReadMeta(*catalog, "CacheUuid");
            const auto storedBuild = Store::ReadMeta(*catalog, "Build");
            if (!uuid) return std::unexpected(uuid.error());
            if (!storedBuild) return std::unexpected(storedBuild.error());
            if (*uuid != cache.Uuid() || *storedBuild != build)
            {
                return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "BuildFacts", catalogPath.string(),
                            "the catalog identity does not match this cache and build",
                            "point --store at this build's own directory");
            }
        }

        BuildReport report;
        if (options.Images.empty())
        {
            return Fail(DiagnosticCode::Usage, Severity::NotVerified, "BuildFacts", "--images",
                        "no images were requested", "pass --images or --towers");
        }

        std::unordered_map<std::string, int> seen;
        std::unordered_set<std::string> selected;
        std::vector<WorkItem> items;
        for (const auto& [requestedPath, tower] : options.Images)
        {
            const auto found = cache.FindImage(requestedPath);
            if (!found)
            {
                report.Failed.push_back({requestedPath, Severity::Failed, found.error().Format()});
                continue;
            }
            const std::string path = (*found)->Path;
            if (!selected.insert(path).second) continue;

            std::string file;
            auto existing = catalog->Prepare("SELECT Name FROM Image WHERE Path = ?1");
            if (!existing) return std::unexpected(existing.error());
            if (auto ok = existing->Bind(1, path); !ok) return std::unexpected(ok.error());
            const auto row = existing->Step();
            if (!row) return std::unexpected(row.error());
            if (*row)
            {
                file = existing->Text(0);
            }
            const auto factsDone = HasCompletedFacts(*catalog, path);
            if (!factsDone) return std::unexpected(factsDone.error());
            if (file.empty())
            {
                const std::string base = BasenameOf(path);
                const int count = ++seen[base];
                file = count == 1 ? base + ".db" : base + "-" + std::to_string(count) + ".db";
            }
            const auto storePath = options.Store / "Images" / file;
            const auto valid = ValidImageStore(storePath, path, cache.Uuid());
            if (!valid) return std::unexpected(valid.error());
            if (options.Resume && *factsDone && *valid) continue;
            items.push_back({path, tower, (*found)->Header, storePath, file, *factsDone && *valid});
        }

        std::queue<std::size_t> pending;
        std::mutex queueMutex;
        std::mutex catalogMutex;
        std::vector<WorkResult> results;
        results.reserve(items.size());
        for (auto& item : items)
        {
            const auto path = item.Path;
            results.push_back({std::move(item), false, std::nullopt, 0, 0, 0, 0, 0,
                Diagnostic{DiagnosticCode::Io, Severity::NotVerified, "BuildFacts::Worker", path,
                    "an exception escaped image processing", "inspect the image and retry", {}}});
            pending.push(results.size() - 1);
        }
        std::atomic<bool> resourceStop = false;
        std::atomic<bool> workerEntryFailed = false;

        const auto record = [&](const WorkResult& result) -> Expected<void> {
            if (!result.Ok && result.Item.HadUsableStore) return {};
            std::lock_guard<std::mutex> lock(catalogMutex);
            auto transaction = Store::Transaction::Begin(*catalog);
            if (!transaction) return std::unexpected(transaction.error());
            auto image = catalog->Prepare("INSERT OR REPLACE INTO Image(Path, Name, Tower, Header, State, Reason, FactsVersion) "
                                          "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
            if (!image) return std::unexpected(image.error());
            if (auto ok = image->Bind(1, result.Item.Path); !ok) return ok;
            if (auto ok = image->Bind(2, result.Item.Basename); !ok) return ok;
            if (result.Item.Tower.empty()) { if (auto ok = image->BindNull(3); !ok) return ok; }
            else if (auto ok = image->Bind(3, result.Item.Tower); !ok) return ok;
            if (auto ok = image->Bind(4, static_cast<std::int64_t>(result.Item.Header)); !ok) return ok;
            if (auto ok = image->Bind(5, std::string_view(result.Ok ? "FactsDone" : "FactsFailed")); !ok) return ok;
            if (result.Error) { if (auto ok = image->Bind(6, result.Error->Format()); !ok) return ok; }
            else if (auto ok = image->BindNull(6); !ok) return ok;
            if (result.Ok) { if (auto ok = image->Bind(7, std::string_view(SHERLOCK_VERSION)); !ok) return ok; }
            else if (auto ok = image->BindNull(7); !ok) return ok;
            if (auto step = image->Step(); !step) return std::unexpected(step.error());
            if (result.Ok)
            {
                auto coverage = catalog->Prepare("INSERT OR REPLACE INTO Coverage(Image, Layer, Unit, Read, Total) "
                                                 "VALUES(?1, 'Facts', 'instructions', ?2, ?3)");
                if (!coverage) return std::unexpected(coverage.error());
                if (auto ok = coverage->Bind(1, result.Item.Path); !ok) return ok;
                if (auto ok = coverage->Bind(2, static_cast<std::int64_t>(result.Decoded)); !ok) return ok;
                if (auto ok = coverage->Bind(3, static_cast<std::int64_t>(result.Total)); !ok) return ok;
                if (auto step = coverage->Step(); !step) return std::unexpected(step.error());
            }
            else
            {
                auto coverage = catalog->Prepare("DELETE FROM Coverage WHERE Image = ?1 AND Layer = 'Facts'");
                if (!coverage) return std::unexpected(coverage.error());
                if (auto ok = coverage->Bind(1, result.Item.Path); !ok) return ok;
                if (auto step = coverage->Step(); !step) return std::unexpected(step.error());
            }
            return transaction->Commit();
        };

        const unsigned requestedWorkers = options.Workers != 0 ? options.Workers : std::max(1u, std::thread::hardware_concurrency());
        const unsigned workerCount = std::min<unsigned>(requestedWorkers, static_cast<unsigned>(items.size()));
        std::vector<std::jthread> workers;
        try
        {
            workers.reserve(workerCount);
            for (unsigned w = 0; w < workerCount; ++w)
            {
                workers.emplace_back([&]() {
                    try
                    {
                        for (;;)
                        {
                        std::size_t index = 0;
                        {
                            std::lock_guard<std::mutex> lock(queueMutex);
                            if (pending.empty() || resourceStop) return;
                            index = pending.front();
                            pending.pop();
                        }
                        auto& result = results[index];
                        const auto& item = result.Item;
                        const auto started = std::chrono::steady_clock::now();
                        try
                        {
                            std::error_code spaceError;
                            const auto space = std::filesystem::space(options.Store, spaceError);
                            if (spaceError || space.available < options.MinimumFreeBytes)
                            {
                                result.Error = Diagnostic{DiagnosticCode::Io, Severity::NotVerified, "BuildFacts",
                                    options.Store.string(), "free disk space is below the configured floor",
                                    "free corpus disk space or lower the configured floor",
                                    spaceError ? spaceError.message() : std::string{}};
                                resourceStop = true;
                            }
                            else
                            {
                                auto disassembler = Disassembler::Create();
                                if (!disassembler) result.Error = disassembler.error();
                                else
                                {
                                    auto facts = ExtractImage(cache, {item.Path, item.Header}, *disassembler);
                                    if (!facts) result.Error = facts.error();
                                    else
                                    {
                                        auto temporary = item.StorePath;
                                        temporary += ".building";
                                        std::filesystem::remove(temporary, spaceError);
                                        std::filesystem::remove(temporary.string() + "-wal", spaceError);
                                        std::filesystem::remove(temporary.string() + "-shm", spaceError);
                                        {
                                            auto db = Store::Database::Open(temporary, Store::Database::Mode::ReadWrite);
                                            if (!db) result.Error = db.error();
                                            else if (auto created = Store::CreateImageStore(*db, item.Path, cache.Uuid()); !created) result.Error = created.error();
                                            else if (auto written = WriteImageFacts(*db, *facts, options.Demangle); !written) result.Error = written.error();
                                        }
                                        if (!result.Error)
                                        {
                                            if (auto replaced = ReplaceFile(temporary, item.StorePath); !replaced) result.Error = replaced.error();
                                            else
                                            {
                                                result.Ok = true;
                                                result.Decoded = facts->Coverage.Decoded;
                                                result.Total = facts->Coverage.Total;
                                                result.Bytes = std::filesystem::file_size(item.StorePath, spaceError);
                                            }
                                        }
                                    }
                                }
                            }
                            result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                            PROCESS_MEMORY_COUNTERS counters{};
                            if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)))
                                result.PeakMiB = static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
                            if (result.Error && result.Error->Level == Severity::NotVerified &&
                                result.Error->Code == DiagnosticCode::Io)
                                resourceStop = true;
                            if (const auto stored = record(result); !stored)
                            {
                                result.Ok = false;
                                result.Error = stored.error();
                                if (result.Error->Level == Severity::NotVerified && result.Error->Code == DiagnosticCode::Io)
                                    resourceStop = true;
                            }
                            if (result.Ok && !options.Json)
                            {
                                std::printf("%s  FactsDone  coverage %llu/%llu  %llu bytes  %.1fs  peak %.1f MiB\n",
                                    item.Path.c_str(), static_cast<unsigned long long>(result.Decoded),
                                    static_cast<unsigned long long>(result.Total), static_cast<unsigned long long>(result.Bytes),
                                    result.Seconds, result.PeakMiB);
                            }
                        }
                        catch (const std::exception&)
                        {
                            result.Ok = false;
                            result.Error.emplace(std::move(result.Emergency));
                            result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                            resourceStop = true;
                        }
                        catch (...)
                        {
                            result.Ok = false;
                            result.Error.emplace(std::move(result.Emergency));
                            result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                            resourceStop = true;
                        }
                        }
                    }
                    catch (...)
                    {
                        resourceStop = true;
                        workerEntryFailed = true;
                    }
                });
            }
        }
        catch (const std::exception& error)
        {
            resourceStop = true;
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildFacts", "workers",
                        "worker creation failed", "reduce --workers and retry", error.what());
        }
        workers.clear();
        if (workerEntryFailed)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildFacts::Worker", "worker entry",
                        "an exception escaped the worker loop", "reduce --workers and retry");
        }

        for (const auto& result : results)
        {
            if (result.Ok)
            {
                ++report.Done;
                report.Completed.push_back(
                    {result.Item.Path, result.Bytes, result.Seconds, result.Decoded, result.Total, result.PeakMiB});
            }
            else if (result.Error) report.Failed.push_back({result.Item.Path, result.Error->Level, result.Error->Format()});
        }
        return report;
    }
}
