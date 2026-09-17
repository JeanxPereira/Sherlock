// Sherlock — tools/Sherlock/Source/SherlockCli/Main.cpp
// Entry point: --version and `build facts` today; Task 7 adds the query commands.
#include <DyldSharedCache/Cache.h>
#include <Facts/Builder.h>
#include <SherlockCli/Arguments.h>
#include <SherlockCli/Demangler.h>
#include <SherlockCli/Towers.h>

#include <windows.h>

#include <psapi.h>

#include <chrono>
#include <cstdio>
#include <memory>

using namespace Sherlock;

namespace
{
    int RunBuildFacts(const Cli::Invocation& invocation)
    {
        auto build = Cli::ReadTowersBuild(invocation.Towers);
        if (!build)
        {
            std::printf("NOT VERIFIED: %s\n", build.error().Format().c_str());
            return 2;
        }
        auto cache = DyldSharedCache::Cache::Open(invocation.Cache);
        if (!cache)
        {
            std::printf("NOT VERIFIED: %s\n", cache.error().Format().c_str());
            return 2;
        }
        auto towers = Cli::ReadTowers(invocation.Towers);
        if (!towers)
        {
            std::printf("NOT VERIFIED: %s\n", towers.error().Format().c_str());
            return 2;
        }

        Facts::BuildOptions options;
        options.Store   = invocation.Store;
        options.Workers = invocation.Workers;
        options.Resume  = invocation.Resume;

        auto demangler = Cli::Demangler::Load(Cli::DefaultDemanglerPath());
        if (demangler)
        {
            auto shared      = std::make_shared<Cli::Demangler>(std::move(*demangler));
            options.Demangle = [shared](std::string_view symbol) { return shared->Demangle(symbol); };
        }
        else
        {
            options.Demangle = [](std::string_view) { return std::nullopt; };
        }

        if (invocation.AllTowers)
        {
            for (const auto& tower : *towers)
            {
                options.Images.push_back({tower.Path, tower.Tower});
            }
        }
        else
        {
            for (const auto& image : invocation.Images)
            {
                options.Images.push_back({image, ""});
            }
        }

        const auto start   = std::chrono::steady_clock::now();
        auto       report  = Facts::BuildFacts(*cache, *build, options);
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        PROCESS_MEMORY_COUNTERS counters{};
        ::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters));
        const double peakMiB = static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);

        if (!report)
        {
            std::printf("NOT VERIFIED: %s\n", report.error().Format().c_str());
            return 2;
        }
        for (const auto& [path, reason] : report->Failed)
        {
            std::printf("FAILED  %s  %s\n", path.c_str(), reason.c_str());
        }
        std::printf("built %zu image(s), %zu failed, %.1fs, peak %.1f MiB\n", report->Done, report->Failed.size(),
                   elapsed, peakMiB);
        return report->Failed.empty() ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    auto invocation = Cli::ParseArguments(argc, argv);
    if (!invocation)
    {
        std::printf("NOT VERIFIED: %s\n", invocation.error().Format().c_str());
        return 2;
    }
    if (invocation->Command == "version")
    {
        std::printf("Sherlock %s\n", SHERLOCK_VERSION);
        return 0;
    }
    if (invocation->Command == "build" && !invocation->Positional.empty() && invocation->Positional.front() == "facts")
    {
        return RunBuildFacts(*invocation);
    }
    std::printf("NOT VERIFIED: command %s is not implemented in this build\n", invocation->Command.c_str());
    return 2;
}
