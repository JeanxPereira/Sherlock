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
    std::string JsonEscape(std::string_view text)
    {
        std::string escaped;
        for (const char ch : text)
        {
            switch (ch)
            {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += ch; break;
            }
        }
        return escaped;
    }

    void PrintVerdict(const Cli::Verdict& verdict, bool json)
    {
        if (!json)
        {
            std::printf("%s\n", Cli::FormatVerdict(verdict).c_str());
            return;
        }
        const char* kind = verdict.Kind == Cli::VerdictKind::Found ? "FOUND" :
                           verdict.Kind == Cli::VerdictKind::Empty ? "EMPTY" :
                           verdict.Kind == Cli::VerdictKind::Partial ? "PARTIAL" : "NOT VERIFIED";
        const auto why = JsonEscape(verdict.Why);
        std::printf("{\"verdict\":\"%s\",\"count\":%zu,\"coverage\":{\"read\":%llu,\"total\":%llu},\"reason\":\"%s\"}\n",
                    kind, verdict.Count, static_cast<unsigned long long>(verdict.Read),
                    static_cast<unsigned long long>(verdict.Total), why.c_str());
    }

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
        if (invocation.Cache.empty() || invocation.Store.empty())
        {
            PrintVerdict({Cli::VerdictKind::NotVerified, 0,
                          "build facts requires --cache and --store (or their SHERLOCK_* variables)", 0, 0},
                         invocation.Json);
            return 2;
        }
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
        options.MinimumFreeBytes = invocation.MinimumFreeBytes;
        options.Resume  = invocation.Resume;
        options.Json    = invocation.Json;

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
            PrintVerdict({Cli::VerdictKind::NotVerified, 0, report.error().Format(), 0, 0}, invocation.Json);
            return 2;
        }
        bool notVerified = false;
        for (const auto& failure : report->Failed)
        {
            notVerified = notVerified || failure.Level == Foundation::Severity::NotVerified;
            if (!invocation.Json)
                std::printf("%s  %s  %s\n", failure.Level == Foundation::Severity::NotVerified ? "NOT VERIFIED" : "FAILED",
                            failure.Path.c_str(), failure.Reason.c_str());
        }
        if (invocation.Json)
        {
            std::printf("{\"built\":%zu,\"failed\":%zu,\"elapsed_seconds\":%.3f,\"peak_mib\":%.3f,\"not_verified\":%s,\"images\":[",
                        report->Done, report->Failed.size(), elapsed, peakMiB, notVerified ? "true" : "false");
            for (std::size_t i = 0; i < report->Completed.size(); ++i)
            {
                const auto& image = report->Completed[i];
                const auto path = JsonEscape(image.Path);
                std::printf("%s{\"path\":\"%s\",\"bytes\":%llu,\"seconds\":%.3f,\"coverage\":{\"read\":%llu,\"total\":%llu}}",
                            i == 0 ? "" : ",", path.c_str(), static_cast<unsigned long long>(image.Bytes), image.Seconds,
                            static_cast<unsigned long long>(image.Decoded), static_cast<unsigned long long>(image.Total));
            }
            std::printf("]}\n");
        }
        else
            std::printf("built %zu image(s), %zu failed, %.1fs, peak %.1f MiB\n", report->Done, report->Failed.size(),
                        elapsed, peakMiB);
        return report->Failed.empty() ? 0 : (notVerified ? 2 : 1);
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

    const bool queryCommand = invocation->Command == "q" || invocation->Command == "callers" ||
                              invocation->Command == "calls" || invocation->Command == "refs";
    if ((queryCommand || invocation->Command == "status") && invocation->Store.empty())
    {
        PrintVerdict({Cli::VerdictKind::NotVerified, 0, "no store path; pass --store or set SHERLOCK_STORE", 0, 0},
                     invocation->Json);
        return 2;
    }
    if ((invocation->Command == "q" || invocation->Command == "calls" || invocation->Command == "refs") &&
        invocation->Cache.empty())
    {
        PrintVerdict({Cli::VerdictKind::NotVerified, 0, "no cache path; pass --cache or set SHERLOCK_CACHE", 0, 0},
                     invocation->Json);
        return 2;
    }
    if (queryCommand && invocation->Positional.empty())
    {
        PrintVerdict({Cli::VerdictKind::NotVerified, 0, "no address or symbol given", 0, 0}, invocation->Json);
        return 2;
    }

    Cli::QueryEnvironment env{invocation->Store, invocation->Full, invocation->Json};
    const auto printHeader = [&]() {
        const auto header = Cli::PrintHeader(env, "");
        if (header)
        {
            return true;
        }
        const Cli::Verdict verdict{Cli::VerdictKind::NotVerified, 0, header.error().Format(), 0, 0};
        PrintVerdict(verdict, invocation->Json);
        return false;
    };
    if (invocation->Command == "status")
    {
        if (!printHeader())
        {
            return 2;
        }
        const auto verdict = Cli::RunStatus(env);
        if (!invocation->Json) std::printf("demangler: %s\n",
                    Cli::Demangler::Load(Cli::DefaultDemanglerPath()).has_value() ? "available" : "absent");
        PrintVerdict(verdict, invocation->Json);
        return Cli::ExitCode(verdict);
    }
    if (invocation->Command == "q" || invocation->Command == "callers" || invocation->Command == "calls" ||
        invocation->Command == "refs")
    {
        if (!printHeader())
        {
            return 2;
        }
        if (invocation->Command == "callers")
        {
            const auto address = ParseHexAddress(invocation->Positional.front());
            if (!address)
            {
                PrintVerdict({Cli::VerdictKind::NotVerified, 0, "invalid address " + invocation->Positional.front(), 0, 0}, invocation->Json);
                return 2;
            }
            const auto verdict = Cli::RunCallers(env, *address);
            PrintVerdict(verdict, invocation->Json);
            return Cli::ExitCode(verdict);
        }
        auto cache = DyldSharedCache::Cache::Open(invocation->Cache);
        if (!cache)
        {
            PrintVerdict({Cli::VerdictKind::NotVerified, 0, cache.error().Format(), 0, 0}, invocation->Json);
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
                PrintVerdict({Cli::VerdictKind::NotVerified, 0, "invalid address " + invocation->Positional.front(), 0, 0}, invocation->Json);
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
        PrintVerdict(verdict, invocation->Json);
        return Cli::ExitCode(verdict);
    }

    std::printf("NOT VERIFIED: command %s is not implemented in this build\n", invocation->Command.c_str());
    return 2;
}
