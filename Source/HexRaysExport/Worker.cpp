// Sherlock — Source/HexRaysExport/Worker.cpp
// The one translation unit that includes the IDA SDK: an image opened, every function
// decompiled, one store written (derived).

#include <HexRaysExport/Worker.h>

#include <HexRaysExport/Store.h>
#include <Store/Database.h>

#include <pro.h>
#include <ida.hpp>
#include <funcs.hpp>
#include <lines.hpp>
#include <name.hpp>
#include <idalib.hpp>
#include <hexrays.hpp>

#include <chrono>

#include <windows.h>
#include <format>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Sherlock::HexRaysExport
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        double Since(const std::chrono::steady_clock::time_point& start)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }

        std::string IdaVersionText()
        {
            int major = 0, minor = 0, build = 0;
            get_library_version(major, minor, build);
            return std::format("{}.{}.{}", major, minor, build);
        }

        enum class Unpacked
        {
            None,      // no unpacked database beside the image
            HeldOpen,  // a live process has it: opening it here would fight that session
            LeftBehind // a dead run's debris: nobody holds it, and IDA will not resume from it
        };

        // IDA writes its database next to the input, and an interrupted run leaves that database
        // unpacked. The files alone cannot tell a live session from debris, and the two need
        // opposite answers -- one is "wait for the human", the other is "delete these and rerun".
        // What tells them apart is whether the .id0 can be opened for exclusive write.
        Unpacked UnpackedDatabase(const std::filesystem::path& image, std::filesystem::path& witness)
        {
            for (const char* extension : {".id0", ".id1", ".id2", ".nam", ".til"})
            {
                std::error_code ec;
                const auto candidate = std::filesystem::path(image).replace_extension(extension);
                if (!std::filesystem::exists(candidate, ec))
                {
                    continue;
                }
                witness = candidate;
                HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                {
                    return GetLastError() == ERROR_SHARING_VIOLATION ? Unpacked::HeldOpen
                                                                     : Unpacked::LeftBehind;
                }
                CloseHandle(handle);
                return Unpacked::LeftBehind;
            }
            return Unpacked::None;
        }

        // Beside the image for as long as this worker holds a database open, and gone once it
        // closes cleanly. It is the only thing that separates our own interrupted run's debris
        // from an interactive session's crashed database: on disk the two read identically, and
        // the second is somebody's analysis that IDA can still recover.
        std::filesystem::path RunMarker(const std::filesystem::path& image)
        {
            return std::filesystem::path(image).replace_extension(".sherlock-run");
        }

        // pro.h poisons the stdio that the standard file streams are built on -- the same reason
        // this file prints with qprintf -- so the marker is written through the Win32 call.
        bool WriteMarker(const std::filesystem::path& marker, std::string_view text)
        {
            HANDLE handle = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD written = 0;
            const bool ok =
                WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0;
            CloseHandle(handle);
            return ok;
        }

        void RemoveUnpacked(const std::filesystem::path& image)
        {
            std::error_code ec;
            for (const char* extension : {".id0", ".id1", ".id2", ".nam", ".til"})
            {
                std::filesystem::remove(std::filesystem::path(image).replace_extension(extension), ec);
            }
        }

        // Hex-Rays lines carry IDA's own colour tags. The stored text is what `fn` prints, so the
        // tags come off here rather than at every reader.
        std::string PlainText(cfunc_t& function, std::uint32_t& lines)
        {
            const strvec_t& pseudocode = function.get_pseudocode();
            std::string out;
            qstring plain;
            for (const auto& line : pseudocode)
            {
                plain.qclear();
                tag_remove(&plain, line.line);
                out.append(plain.c_str(), plain.length());
                out.push_back('\n');
            }
            lines = static_cast<std::uint32_t>(pseudocode.size());
            return out;
        }
    }

    std::string WorkerReport::Format() const
    {
        return std::format("{}functions={} decompiled={} too-big={} failed={} lines={} carried={} "
                           "image-bytes={} store-bytes={} open-seconds={:.2f} seconds={:.2f} ida={}",
                           Resumed ? "resumed " : "", Functions, Decompiled, TooBig, Failures, Lines,
                           Carried, ImageBytes, StoreBytes, OpenSeconds, Seconds, IdaVersion);
    }

    Expected<WorkerReport> RunWorker(const WorkerOptions& options)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(options.Image, ec))
        {
            return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "HexRaysExport::RunWorker",
                        options.Image.string(), "the image is not a file",
                        "extract the image into the corpus before exporting it");
        }
        std::filesystem::path witness;
        switch (UnpackedDatabase(options.Image, witness))
        {
        case Unpacked::HeldOpen:
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "HexRaysExport::RunWorker",
                        options.Image.string(),
                        std::format("a live process holds {}", witness.filename().string()),
                        "close the IDA session holding it, or export a copy of the image");
        case Unpacked::LeftBehind:
            // Deleted here only when it is ours to delete: the exclusive-write probe has already
            // proven nobody holds these files, our marker says the run that left them was this
            // worker, and --resume says the caller means to continue it. Without the marker the
            // database belongs to someone's IDA session and the refusal stands -- a resume that
            // cannot clear its own debris is a resume that never runs twice.
            if (options.Resume && std::filesystem::exists(RunMarker(options.Image), ec))
            {
                RemoveUnpacked(options.Image);
                break;
            }
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "HexRaysExport::RunWorker",
                        options.Image.string(),
                        std::format("an interrupted run left {} behind and nothing holds it",
                                    witness.filename().string()),
                        "delete the .id0/.id1/.nam/.til beside the image and run this again");
        case Unpacked::None:
            break;
        }

        WorkerReport report;
        report.IdaVersion = IdaVersionText();
        report.ImageBytes = std::filesystem::file_size(options.Image, ec);

        std::filesystem::create_directories(options.StoreDir, ec);
        const auto storePath = StorePath(options.StoreDir, options.ImageName);

        // --resume mirrors `build facts --resume`, with one more state than that command has.
        // A store that says Complete costs nothing on the next run; a partial one is continued
        // at the function it did not reach; one that is unreadable, of the wrong schema or empty
        // is built again from the first function.
        std::vector<std::uint64_t> already;
        if (std::filesystem::exists(storePath, ec))
        {
            bool carry = false;
            if (options.Resume)
            {
                auto opened = Store::Database::Open(storePath, Store::Database::Mode::ReadOnly);
                if (opened && CheckStoreSchema(*opened))
                {
                    auto complete = IsComplete(*opened);
                    auto coverage = ReadCoverage(*opened);
                    if (complete && coverage && coverage->Attempted > 0)
                    {
                        if (*complete)
                        {
                            report.Resumed    = true;
                            report.Functions  = coverage->Attempted;
                            report.Decompiled = coverage->Decompiled;
                            report.TooBig     = coverage->TooBig;
                            report.Failures   = coverage->Attempted - coverage->Decompiled - coverage->TooBig;
                            report.Lines      = coverage->Lines;
                            report.Carried    = coverage->Attempted;
                            report.StoreBytes = std::filesystem::file_size(storePath, ec);
                            return report;
                        }
                        if (auto stored = ReadStoredFunctions(*opened))
                        {
                            already = std::move(*stored);
                            carry   = true;
                        }
                    }
                }
            }
            if (!carry)
            {
                // The sidecars go with it. A -wal left by a killed process is replayed into the
                // next database opened under that name, which would put rows behind a store the
                // caller asked to start clean.
                std::filesystem::remove(storePath, ec);
                std::filesystem::remove(std::filesystem::path(storePath) += "-wal", ec);
                std::filesystem::remove(std::filesystem::path(storePath) += "-shm", ec);
            }
        }
        report.Carried = already.size();

        // Written before the database exists, because IDA unpacks it during open_database and a
        // run killed inside that call leaves debris the next run has to recognise as ours.
        const auto marker = RunMarker(options.Image);
        if (!WriteMarker(marker, options.ImageName))
        {
            // Refused rather than run without it: IDA is about to write a multi-gigabyte database
            // into this same directory, and a run whose debris cannot be told from an interactive
            // session's is a run that can never resume after an interruption.
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "HexRaysExport::RunWorker",
                        marker.string(), "the run marker could not be written beside the image",
                        "give the corpus directory write permission and run this again");
        }

        const auto openedAt = std::chrono::steady_clock::now();
        if (open_database(options.Image.string().c_str(), /*run_auto=*/true) != 0)
        {
            std::filesystem::remove(marker, ec);
            return Fail(DiagnosticCode::Io, Severity::Failed, "HexRaysExport::RunWorker",
                        options.Image.string(), "open_database refused the image",
                        "check that the file is a Mach-O image IDA can load");
        }
        report.OpenSeconds = Since(openedAt);

        if (!init_hexrays_plugin())
        {
            close_database(false);
            std::filesystem::remove(marker, ec);
            return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "HexRaysExport::RunWorker",
                        options.Image.string(),
                        "the decompiler did not answer with a database open",
                        "point SHERLOCK_IDA_SDK at the SDK whose HEXRAYS_API_MAGIC the installed "
                        "decompiler carries");
        }

        const auto startedAt = std::chrono::steady_clock::now();
        const std::size_t quantity = get_func_qty();
        report.Functions = quantity;

        // Held in an optional so the connection can be closed by hand before the store is
        // measured: Database closes in its destructor, and the size of an open one is a lie.
        std::optional<Store::Database> store;
        {
            auto opened = Store::Database::Open(storePath, Store::Database::Mode::ReadWrite);
            if (!opened)
            {
                close_database(false);
                std::filesystem::remove(marker, ec);
                return std::unexpected(opened.error());
            }
            store.emplace(std::move(*opened));
        }
        if (auto ok = CreateStore(*store, options.ImagePath, options.Build, report.IdaVersion); !ok)
        {
            close_database(false);
            std::filesystem::remove(marker, ec);
            return std::unexpected(ok.error());
        }

        const std::unordered_set<std::uint64_t> carried(already.begin(), already.end());

        std::vector<DecompilationRow> rows;
        std::vector<NameRow> names;
        const auto batch = static_cast<std::size_t>(options.BatchSize == 0 ? 1 : options.BatchSize);
        rows.reserve(batch);

        auto flush = [&]() -> Expected<void> {
            if (rows.empty() && names.empty())
            {
                return {};
            }
            auto ok = WriteRows(*store, rows, names);
            rows.clear();
            names.clear();
            return ok;
        };

        qstring name;
        for (std::size_t index = 0; index < quantity; ++index)
        {
            func_t* function = getn_func(index);
            if (function == nullptr)
            {
                continue;
            }
            const auto address = static_cast<std::uint64_t>(function->start_ea);
            if (carried.contains(address))
            {
                continue;
            }

            if (get_func_name(&name, function->start_ea) > 0 && name.length() > 0)
            {
                names.push_back({address, std::string(name.c_str(), name.length())});
            }

            // The 9.2 SDK exposes no size limit to check ahead of the call: the decompiler owns
            // that threshold and reports crossing it as MERR_FUNCSIZE. The row records what came
            // back rather than a guess at where the limit sits.
            hexrays_failure_t failure;
            const auto functionStartedAt = std::chrono::steady_clock::now();
            cfuncptr_t decompiled = decompile(function, &failure, DECOMP_NO_WAIT | DECOMP_NO_CACHE);
            const double seconds = Since(functionStartedAt);

            if (decompiled == nullptr)
            {
                qstring description = failure.desc();
                std::string reason =
                    failure.code == MERR_FUNCSIZE
                        ? std::string(kTooBigReason)
                        : (description.empty() ? std::format("merror {}", int(failure.code))
                                               : std::string(description.c_str(), description.length()));
                rows.push_back({address, {}, 0, Status::Failed, std::move(reason), seconds});
            }
            else
            {
                std::uint32_t lines = 0;
                std::string text = PlainText(*decompiled, lines);
                rows.push_back({address, std::move(text), lines, Status::Ok, {}, seconds});
            }

            if (rows.size() >= batch)
            {
                if (auto ok = flush(); !ok)
                {
                    close_database(false);
                    std::filesystem::remove(marker, ec);
                    return std::unexpected(ok.error());
                }
            }
        }
        report.Seconds = Since(startedAt);

        // Nothing learned here belongs in the .i64: the store is the output, and a saved database
        // would differ from the one an interactive session expects. Closed before the last write
        // so the gigabytes IDA holds are gone while SQLite finishes.
        close_database(false);

        if (auto ok = flush(); !ok)
        {
            std::filesystem::remove(marker, ec);
            return std::unexpected(ok.error());
        }
        if (auto ok = MarkComplete(*store); !ok)
        {
            std::filesystem::remove(marker, ec);
            return std::unexpected(ok.error());
        }

        // Read back rather than tallied along the way: a resumed run attempts only the functions
        // its predecessor did not reach, so counters it kept would describe the last slice and
        // print it as the image.
        auto coverage = ReadCoverage(*store);
        if (!coverage)
        {
            std::filesystem::remove(marker, ec);
            return std::unexpected(coverage.error());
        }
        report.Decompiled = coverage->Decompiled;
        report.TooBig     = coverage->TooBig;
        report.Failures   = coverage->Attempted - coverage->Decompiled - coverage->TooBig;
        report.Lines      = coverage->Lines;

        store.reset();
        std::filesystem::remove(marker, ec);

        // Measured only after the connection closes. Under WAL the rows sit in the -wal file
        // until a checkpoint, and SQLite checkpoints on its own once that file passes about
        // 4 MB -- so measuring while the database is open reported the real size for a large
        // image and 4096 bytes, one empty page, for every store smaller than that.
        report.StoreBytes = std::filesystem::file_size(storePath, ec);

        if (!options.KeepDatabase)
        {
            std::filesystem::remove(std::filesystem::path(options.Image).replace_extension(".i64"), ec);
        }
        return report;
    }
}
