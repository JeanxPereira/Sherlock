// Sherlock — tools/Sherlock/Source/HexRaysExport/include/HexRaysExport/Worker.h
// One image in, one layer-2 store out: the worker's options and what it reports (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Sherlock::HexRaysExport
{
    using Foundation::Expected;

    // No IDA type appears here: the parent spawns this work as a process, and nothing that links
    // the SDK is needed to describe it.
    struct WorkerOptions
    {
        std::filesystem::path Image;     // the file IDA opens
        std::filesystem::path StoreDir;  // <corpus>/<Build>/Sherlock/Images
        std::string           ImageName; // the catalog's Image.Name
        std::string           ImagePath; // the catalog's Image.Path, cache-relative
        std::string           Build;
        bool                  KeepDatabase = false;  // a tower's .i64 stays for interactive use
        bool                  Resume       = false;  // a store already holding rows is left alone
    };

    struct WorkerReport
    {
        std::uint64_t  Functions   = 0;
        std::uint64_t  Decompiled  = 0;
        std::uint64_t  TooBig      = 0;
        std::uint64_t  Failures    = 0;
        std::uint64_t  Lines       = 0;
        std::uintmax_t ImageBytes  = 0;
        std::uintmax_t StoreBytes  = 0;
        double         OpenSeconds = 0;
        double         Seconds     = 0;
        std::string    IdaVersion;
        bool           Resumed = false;  // the store was already there and nothing was decompiled

        // "functions=N decompiled=N …", the one line the parent reads off a finished worker.
        std::string Format() const;
    };

    Expected<WorkerReport> RunWorker(const WorkerOptions& options);
}
