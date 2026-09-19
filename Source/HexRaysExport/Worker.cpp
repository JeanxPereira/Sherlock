// Sherlock — tools/Sherlock/Source/HexRaysExport/Worker.cpp
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
#include <format>
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

        // IDA writes its database next to the input. An unpacked database -- the .id0 and friends
        // beside the image -- means a GUI or another process holds it open, and opening it here
        // would fight that session for the same files.
        bool DatabaseIsHeldOpen(const std::filesystem::path& image)
        {
            for (const char* extension : {".id0", ".id1", ".id2", ".nam", ".til"})
            {
                std::error_code ec;
                if (std::filesystem::exists(std::filesystem::path(image).replace_extension(extension), ec))
                {
                    return true;
                }
            }
            return false;
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
        return std::format("{}functions={} decompiled={} too-big={} failed={} lines={} image-bytes={} "
                           "store-bytes={} open-seconds={:.2f} seconds={:.2f} ida={}",
                           Resumed ? "resumed " : "", Functions, Decompiled, TooBig, Failures, Lines,
                           ImageBytes, StoreBytes, OpenSeconds, Seconds, IdaVersion);
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
        if (DatabaseIsHeldOpen(options.Image))
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "HexRaysExport::RunWorker",
                        options.Image.string(), "an unpacked IDA database sits beside this image",
                        "close the IDA session holding it, or export a copy of the image");
        }

        WorkerReport report;
        report.IdaVersion = IdaVersionText();
        report.ImageBytes = std::filesystem::file_size(options.Image, ec);

        // --resume mirrors `build facts --resume`: an image already exported costs nothing on the
        // next run. A store that exists but carries no rows is a crashed run, not progress, and
        // is exported again.
        const auto existing = StorePath(options.StoreDir, options.ImageName);
        if (options.Resume && std::filesystem::exists(existing, ec))
        {
            auto opened = Store::Database::Open(existing, Store::Database::Mode::ReadOnly);
            if (opened && CheckStoreSchema(*opened))
            {
                auto coverage = ReadCoverage(*opened);
                if (coverage && coverage->Attempted > 0)
                {
                    report.Resumed = true;
                    report.Functions = coverage->Attempted;
                    report.Decompiled = coverage->Decompiled;
                    report.StoreBytes = std::filesystem::file_size(existing, ec);
                    return report;
                }
            }
        }

        const auto openedAt = std::chrono::steady_clock::now();
        if (open_database(options.Image.string().c_str(), /*run_auto=*/true) != 0)
        {
            return Fail(DiagnosticCode::Io, Severity::Failed, "HexRaysExport::RunWorker",
                        options.Image.string(), "open_database refused the image",
                        "check that the file is a Mach-O image IDA can load");
        }
        report.OpenSeconds = Since(openedAt);

        if (!init_hexrays_plugin())
        {
            close_database(false);
            return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "HexRaysExport::RunWorker",
                        options.Image.string(),
                        "the decompiler did not answer with a database open",
                        "point SHERLOCK_IDA_SDK at the SDK whose HEXRAYS_API_MAGIC the installed "
                        "decompiler carries");
        }

        const auto startedAt = std::chrono::steady_clock::now();
        const std::size_t quantity = get_func_qty();
        report.Functions = quantity;

        std::vector<DecompilationRow> rows;
        std::vector<NameRow> names;
        rows.reserve(quantity);

        qstring name;
        for (std::size_t index = 0; index < quantity; ++index)
        {
            func_t* function = getn_func(index);
            if (function == nullptr)
            {
                continue;
            }
            const auto address = static_cast<std::uint64_t>(function->start_ea);

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
                const bool tooBig = failure.code == MERR_FUNCSIZE;
                tooBig ? ++report.TooBig : ++report.Failures;
                qstring description = failure.desc();
                rows.push_back({address, {}, 0, Status::Failed,
                                description.empty() ? std::format("merror {}", int(failure.code))
                                                    : std::string(description.c_str(), description.length()),
                                seconds});
                continue;
            }

            std::uint32_t lines = 0;
            std::string text = PlainText(*decompiled, lines);
            report.Lines += lines;
            ++report.Decompiled;
            rows.push_back({address, std::move(text), lines, Status::Ok, {}, seconds});
        }
        report.Seconds = Since(startedAt);

        // Nothing learned here belongs in the .i64: the store is the output, and a saved database
        // would differ from the one an interactive session expects.
        close_database(false);

        std::filesystem::create_directories(options.StoreDir, ec);
        const auto storePath = StorePath(options.StoreDir, options.ImageName);
        std::filesystem::remove(storePath, ec);
        auto store = Store::Database::Open(storePath, Store::Database::Mode::ReadWrite);
        if (!store)
        {
            return std::unexpected(store.error());
        }
        if (auto ok = CreateStore(*store, options.ImagePath, options.Build, report.IdaVersion); !ok)
        {
            return std::unexpected(ok.error());
        }
        if (auto ok = WriteRows(*store, rows, names); !ok)
        {
            return std::unexpected(ok.error());
        }
        report.StoreBytes = std::filesystem::file_size(storePath, ec);

        if (!options.KeepDatabase)
        {
            std::filesystem::remove(std::filesystem::path(options.Image).replace_extension(".i64"), ec);
        }
        return report;
    }
}
