// Sherlock — tools/Sherlock/Source/SherlockCli/HexRays.cpp
// `build hexrays`: image order, the free-space floor, one worker process per image, and the
// catalog states only this process writes (derived).

#include <SherlockCli/HexRays.h>

#include <SherlockCli/Arguments.h>

#include <HexRaysExport/Store.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace Sherlock::Cli
{
    namespace
    {
        struct Target
        {
            std::string Path;   // the catalog's Image.Path, cache-relative
            std::string Name;   // the image itself, the last component of Path -- NOT the catalog's
                                // Image.Name, which is the layer-1 store's file name ("AppKit.db")
            std::string Tower;  // empty when the image belongs to no tower
        };

        std::filesystem::path WorkerPath()
        {
            // The gates drive this with a stand-in worker, so the three behaviours that belong to
            // the parent -- the resume reset, a failed image, the free-space floor -- are provable
            // without an IDA licence and in seconds.
            if (const char* override = std::getenv("SHERLOCK_HEXRAYS_WORKER"); override != nullptr)
            {
                return override;
            }
            wchar_t buffer[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (length == 0)
            {
                return {};
            }
            return std::filesystem::path(std::wstring(buffer, length)).parent_path() / "SherlockHexRays.exe";
        }

        std::uintmax_t FreeBytes(const std::filesystem::path& directory)
        {
            ULARGE_INTEGER available {};
            if (!GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr))
            {
                return 0;
            }
            return available.QuadPart;
        }

        // The worker's imports resolve at process start, so IDA's directory has to be on the PATH
        // the child inherits. Setting it here, once, is the whole of it: a child gets the parent's
        // environment.
        bool PutIdaOnPath(const std::filesystem::path& idaDir)
        {
            if (idaDir.empty())
            {
                return true;
            }
            std::wstring path = idaDir.wstring();
            const DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            if (length > 0)
            {
                std::wstring current(length, L'\0');
                GetEnvironmentVariableW(L"PATH", current.data(), length);
                current.resize(length - 1);
                path += L';';
                path += current;
            }
            return SetEnvironmentVariableW(L"PATH", path.c_str()) != 0;
        }

        std::wstring Quoted(const std::wstring& value)
        {
            return L'"' + value + L'"';
        }

        std::wstring Widen(std::string_view text)
        {
            return std::filesystem::path(text).wstring();
        }

        // Exit 2 from a worker is NOT VERIFIED -- it could not look -- and exit 1 is the image
        // itself failing. The catalog records the two apart, because a rerun fixes one and not
        // the other. The child inherits this process's console, so its own line lands in the log
        // beside this one.
        int RunWorker(const std::filesystem::path& worker, const std::filesystem::path& image,
                      const std::filesystem::path& storeDir, const Target& target,
                      std::string_view build, bool resume)
        {
            std::wstring command = Quoted(worker.wstring());
            command += L" --image " + Quoted(image.wstring());
            command += L" --store " + Quoted(storeDir.wstring());
            command += L" --name " + Quoted(Widen(target.Name));
            command += L" --image-path " + Quoted(Widen(target.Path));
            command += L" --build " + Quoted(Widen(build));
            if (!target.Tower.empty())
            {
                // A tower's .i64 stays for interactive use; every other image's is deleted.
                command += L" --keep-database";
            }
            if (resume)
            {
                command += L" --resume";
            }

            STARTUPINFOW startup { sizeof(startup) };
            PROCESS_INFORMATION process {};
            std::vector<wchar_t> mutableCommand(command.begin(), command.end());
            mutableCommand.push_back(L'\0');
            if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                &startup, &process))
            {
                return 2;
            }
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD code = 2;
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return static_cast<int>(code);
        }

        BuildHexRaysResult NotVerified(std::string why)
        {
            return {{VerdictKind::NotVerified, 0, std::move(why), 0, 0}, 2};
        }
    }

    BuildHexRaysResult BuildHexRays(const Invocation& invocation)
    {
        if (invocation.Store.empty())
        {
            return NotVerified("build hexrays requires --store (or SHERLOCK_STORE)");
        }

        const auto worker = WorkerPath();
        if (worker.empty() || !std::filesystem::exists(worker))
        {
            return NotVerified("layer 2 was not built: configure with -DSHERLOCK_IDA_SDK and "
                               "-DSHERLOCK_IDA_DIR naming an SDK and an IDA whose decompiler APIs match");
        }

        const auto imagesDir = invocation.ImagesDir.empty()
                                   ? invocation.Store.parent_path() / "extracted" / "dylibs"
                                   : invocation.ImagesDir;
        const auto storeDir = invocation.Store / "Images";
        std::error_code ec;
        std::filesystem::create_directories(storeDir, ec);

        auto catalog = Store::Database::Open(invocation.Store / "Catalog.db", Store::Database::Mode::ReadWrite);
        if (!catalog)
        {
            return NotVerified(catalog.error().Format());
        }
        if (auto ok = Store::CheckSchema(*catalog, "Catalog"); !ok)
        {
            return NotVerified(ok.error().Format());
        }
        auto buildId = Store::ReadMeta(*catalog, "Build");
        if (!buildId)
        {
            return NotVerified(buildId.error().Format());
        }

        // An image left HexRaysRunning is a crash, not progress: this process is the only one that
        // spawns workers, so nothing it did not start can still be running.
        if (invocation.Resume)
        {
            if (auto ok = catalog->Execute("UPDATE Image SET State = 'Pending' WHERE State = 'HexRaysRunning'");
                !ok)
            {
                return NotVerified(ok.error().Format());
            }
        }

        // Towers first, then everything else, which is the order §7 asks for and the order that
        // makes an interrupted run leave the useful images done.
        std::vector<Target> targets;
        {
            auto rows = catalog->Prepare("SELECT Path, Name, Tower, State FROM Image "
                                         "ORDER BY Tower IS NULL, Tower, Name");
            if (!rows)
            {
                return NotVerified(rows.error().Format());
            }
            for (;;)
            {
                auto row = rows->Step();
                if (!row)
                {
                    return NotVerified(row.error().Format());
                }
                if (!*row)
                {
                    break;
                }
                const std::string path(rows->Text(0));
                Target target {path, std::filesystem::path(path).filename().string(),
                               rows->IsNull(2) ? std::string{} : std::string(rows->Text(2))};
                const std::string state(rows->Text(3));

                const bool named = std::find(invocation.Images.begin(), invocation.Images.end(), target.Name)
                                   != invocation.Images.end();
                const bool selected = invocation.Images.empty()
                                          ? (!invocation.AllTowers || !target.Tower.empty())
                                          : named;
                if (!selected)
                {
                    continue;
                }
                if (invocation.Resume && state == "HexRaysDone")
                {
                    continue;
                }
                targets.push_back(std::move(target));
            }
        }

        if (targets.empty())
        {
            return {{VerdictKind::Found, 0, "every selected image is already HexRaysDone", 0, 0}, 0};
        }

        if (!PutIdaOnPath(invocation.IdaDir))
        {
            return NotVerified("the IDA directory could not be put on the child's PATH");
        }

        std::mutex catalogLock;  // Catalog.db has exactly one writer, and this is it.
        std::mutex queueLock;
        std::size_t next = 0, done = 0, failed = 0, missing = 0;
        bool floorHit = false;

        const auto setState = [&](const Target& target, std::string_view state, std::string_view reason)
        {
            std::scoped_lock guard(catalogLock);
            auto statement = catalog->Prepare("UPDATE Image SET State = ?2, Reason = ?3, HexRaysVersion = ?4 "
                                              "WHERE Path = ?1");
            if (!statement)
            {
                return;
            }
            (void)statement->Bind(1, target.Path);
            (void)statement->Bind(2, state);
            reason.empty() ? (void)statement->BindNull(3) : (void)statement->Bind(3, reason);
            state == "HexRaysDone" ? (void)statement->Bind(4, std::string_view(SHERLOCK_VERSION))
                                   : (void)statement->BindNull(4);
            (void)statement->Step();
        };

        const auto setCoverage = [&](const Target& target, std::uint64_t read, std::uint64_t total)
        {
            std::scoped_lock guard(catalogLock);
            auto statement = catalog->Prepare("INSERT OR REPLACE INTO Coverage(Image, Layer, Unit, Read, Total) "
                                              "VALUES(?1, 'HexRays', 'functions', ?2, ?3)");
            if (!statement)
            {
                return;
            }
            (void)statement->Bind(1, target.Path);
            (void)statement->Bind(2, static_cast<std::int64_t>(read));
            (void)statement->Bind(3, static_cast<std::int64_t>(total));
            (void)statement->Step();
        };

        const auto pump = [&]
        {
            for (;;)
            {
                Target target;
                {
                    std::scoped_lock guard(queueLock);
                    if (floorHit || next >= targets.size())
                    {
                        return;
                    }
                    // Checked before each image, not once: a worker's own .i64 is what fills the
                    // disk, and the run has to stop before the next one starts.
                    if (FreeBytes(storeDir) < invocation.MinimumFreeBytes)
                    {
                        floorHit = true;
                        return;
                    }
                    target = targets[next++];
                }

                const auto image = imagesDir / target.Name;
                std::error_code exists;
                if (!std::filesystem::is_regular_file(image, exists))
                {
                    // Layer 2 reads a file, and the cache holds images nobody extracted. The row
                    // names the one that is missing instead of the run stopping on it.
                    setState(target, "HexRaysFailed", std::format("no extracted image at {}", image.string()));
                    std::scoped_lock guard(queueLock);
                    ++missing;
                    continue;
                }

                setState(target, "HexRaysRunning", {});
                const int code = RunWorker(worker, image, storeDir, target, *buildId, invocation.Resume);

                if (code == 0)
                {
                    std::uint64_t read = 0, total = 0;
                    auto store = Store::Database::Open(HexRaysExport::StorePath(storeDir, target.Name),
                                                       Store::Database::Mode::ReadOnly);
                    if (store)
                    {
                        if (auto coverage = HexRaysExport::ReadCoverage(*store))
                        {
                            read = coverage->Decompiled;
                            total = coverage->Attempted;
                        }
                    }
                    setState(target, "HexRaysDone", {});
                    setCoverage(target, read, total);
                    std::scoped_lock guard(queueLock);
                    ++done;
                }
                else
                {
                    setState(target, "HexRaysFailed",
                             code == 2 ? "the worker could not look (exit 2); its own line says why"
                                       : "the worker failed on this image (exit 1); its own line says why");
                    std::scoped_lock guard(queueLock);
                    ++failed;
                }
            }
        };

        const unsigned workers = invocation.Workers > 0 ? invocation.Workers : 2u;
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (unsigned i = 0; i < workers; ++i)
        {
            pool.emplace_back(pump);
        }
        for (auto& thread : pool)
        {
            thread.join();
        }

        const auto total = static_cast<std::uint64_t>(targets.size());
        if (floorHit)
        {
            return {{VerdictKind::NotVerified, done,
                     std::format("free space on {} is under the {} byte floor", storeDir.string(),
                                 invocation.MinimumFreeBytes),
                     done, total},
                    2};
        }

        auto why = std::format("{} exported, {} failed, {} without an extracted image", done, failed, missing);
        const bool complete = failed == 0 && missing == 0;
        return {{complete ? VerdictKind::Found : VerdictKind::Partial, done, std::move(why), done, total},
                failed > 0 ? 1 : 0};
    }
}
