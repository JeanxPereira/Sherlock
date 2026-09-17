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
        bool                                                         Resume  = false;
        std::function<std::optional<std::string>(std::string_view)>  Demangle;
    };

    struct BuildReport
    {
        std::size_t                                       Done = 0;
        std::vector<std::pair<std::string, std::string>>  Failed; // path, reason
    };

    Expected<BuildReport> BuildFacts(const DyldSharedCache::Cache& cache, std::string_view build,
                                     const BuildOptions& options);
}
