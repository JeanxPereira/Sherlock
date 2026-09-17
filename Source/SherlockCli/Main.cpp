// Sherlock — tools/Sherlock/Source/SherlockCli/Main.cpp
// Entry point: --version, `build facts`, and the query commands (q, callers, calls, refs, status).
#include <DyldSharedCache/Cache.h>
#include <Facts/Builder.h>
#include <SherlockCli/Arguments.h>
#include <SherlockCli/Demangler.h>
#include <SherlockCli/Queries.h>
#include <SherlockCli/Towers.h>

#include <windows.h>

#include <psapi.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace Sherlock;

namespace
{
    std::optional<std::uint64_t> ParseHexAddress(std::string_view text)
    {
        if (text.empty())
        {
            return std::nullopt;
        }
        const std::string owned(text);
        char*             end = nullptr;
        errno                   = 0;
        const auto address      = std::strtoull(owned.c_str(), &end, 16);
        if (end == owned.c_str() || *end != '\0' || errno == ERANGE)
        {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(address);
    }

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

    Cli::QueryEnvironment env{invocation->Store, invocation->Full, invocation->Json};
    const auto printHeader = [&]() {
        const auto header = Cli::PrintHeader(env, "");
        if (header)
        {
            return true;
        }
        const Cli::Verdict verdict{Cli::VerdictKind::NotVerified, 0, header.error().Format(), 0, 0};
        std::printf("%s\n", Cli::FormatVerdict(verdict).c_str());
        return false;
    };
    if (invocation->Command == "status")
    {
        if (!printHeader())
        {
            return 2;
        }
        const auto verdict = Cli::RunStatus(env);
        std::printf("demangler: %s\n",
                    Cli::Demangler::Load(Cli::DefaultDemanglerPath()).has_value() ? "available" : "absent");
        std::printf("%s\n", Cli::FormatVerdict(verdict).c_str());
        return Cli::ExitCode(verdict);
    }
    if (invocation->Command == "q" || invocation->Command == "callers" || invocation->Command == "calls" ||
        invocation->Command == "refs")
    {
        if (!printHeader())
        {
            return 2;
        }
        if (invocation->Positional.empty())
        {
            std::printf("verdict: NOT VERIFIED no address or symbol given\n");
            return 2;
        }
        if (invocation->Command == "callers")
        {
            const auto address = ParseHexAddress(invocation->Positional.front());
            if (!address)
            {
                std::printf("verdict: NOT VERIFIED invalid address %s\n", invocation->Positional.front().c_str());
                return 2;
            }
            const auto verdict = Cli::RunCallers(env, *address);
            std::printf("%s\n", Cli::FormatVerdict(verdict).c_str());
            return Cli::ExitCode(verdict);
        }
        auto cache = DyldSharedCache::Cache::Open(invocation->Cache);
        if (!cache)
        {
            std::printf("verdict: NOT VERIFIED %s\n", cache.error().Format().c_str());
            return 2;
        }
        Cli::Verdict verdict;
        if (invocation->Command == "q")
        {
            verdict = Cli::RunQuery(*cache, env, invocation->Positional.front());
        }
        else
        {
            const auto address = ParseHexAddress(invocation->Positional.front());
            if (!address)
            {
                std::printf("verdict: NOT VERIFIED invalid address %s\n", invocation->Positional.front().c_str());
                return 2;
            }
            if (invocation->Command == "calls")
            {
                verdict = Cli::RunCalls(*cache, env, *address);
            }
            else
            {
                const std::uint64_t endAddress = invocation->To.value_or(*address + 1);
                verdict = Cli::RunRefs(*cache, env, *address, endAddress);
            }
        }
        std::printf("%s\n", Cli::FormatVerdict(verdict).c_str());
        return Cli::ExitCode(verdict);
    }

    std::printf("NOT VERIFIED: command %s is not implemented in this build\n", invocation->Command.c_str());
    return 2;
}
