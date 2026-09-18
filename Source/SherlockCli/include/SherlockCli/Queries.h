// Sherlock — tools/Sherlock/Source/SherlockCli/include/SherlockCli/Queries.h
// q, callers, calls, refs, status -- each opens the image stores it needs and returns one Verdict (derived).
#pragma once

#include <DyldSharedCache/Cache.h>
#include <Foundation/Diagnostic.h>
#include <SherlockCli/Verdict.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <string>
#include <vector>

namespace Sherlock::Cli
{
    struct QueryEnvironment
    {
        std::filesystem::path Store;
        std::filesystem::path Documents;
        std::filesystem::path Repo;
        bool                  Full = false;
        bool                  Json = false;
        std::vector<std::string>* Output = nullptr;
        std::function<Foundation::Expected<void>(std::string_view)> PlainTextSink;
    };

    Foundation::Expected<void> PrintHeader(const QueryEnvironment& env, std::string_view build);

    Verdict RunQuery(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::string_view target);
    Verdict RunCallers(const QueryEnvironment& env, std::uint64_t address);
    Verdict RunCalls(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t address);
    Verdict RunRefs(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t address,
                    std::uint64_t end);
    Verdict RunStatus(const QueryEnvironment& env);
}
