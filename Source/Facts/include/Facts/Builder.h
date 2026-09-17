// Sherlock — tools/Sherlock/Source/Facts/include/Facts/Builder.h
// The catalog, per-image stores, and the worker threads that fill them (derived).
#pragma once

#include <DyldSharedCache/Cache.h>
#include <Foundation/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Sherlock::Facts
{
    using Foundation::Expected;

    struct BuildOptions
    {
        std::filesystem::path                                       Store;
        std::vector<std::pair<std::string, std::string>>             Images; // path, tower
        unsigned                                                     Workers = 0;
        std::uintmax_t                                                MinimumFreeBytes = 1024ull * 1024ull * 1024ull;
        bool                                                         Resume  = false;
        bool                                                         Json    = false;
        std::function<std::optional<std::string>(std::string_view)>  Demangle;
    };

    struct BuildReport
    {
        struct CompletedImage
        {
            std::string Path;
            std::uintmax_t Bytes = 0;
            double Seconds = 0;
            std::uint64_t Decoded = 0;
            std::uint64_t Total = 0;
        };
        std::size_t                                       Done = 0;
        std::vector<CompletedImage>                       Completed;
        struct Failure
        {
            std::string Path;
            Foundation::Severity Level = Foundation::Severity::Failed;
            std::string Reason;
        };
        std::vector<Failure> Failed;
    };

    Expected<BuildReport> BuildFacts(const DyldSharedCache::Cache& cache, std::string_view build,
                                     const BuildOptions& options);
}
