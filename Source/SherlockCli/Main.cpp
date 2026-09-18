// Sherlock — tools/Sherlock/Source/SherlockCli/Main.cpp
// Entry point: --version, `build facts`, and the query commands (q, callers, calls, refs, status).
#include <DyldSharedCache/Cache.h>
#include <DocumentIndex/Builder.h>
#include <Facts/Builder.h>
#include <SherlockCli/Arguments.h>
#include <SherlockCli/Demangler.h>
#include <SherlockCli/Queries.h>
#include <SherlockCli/Towers.h>

#include <nlohmann/json.hpp>

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
#include <vector>

using namespace Sherlock;

namespace
{
    void PrintVerdict(const Cli::Verdict& verdict, bool json, const std::vector<std::string>& output = {})
    {
        if (!json)
        {
            std::printf("%s\n", Cli::FormatVerdict(verdict).c_str());
            return;
        }
        const auto effective = Cli::EffectiveKind(verdict);
        const char* kind = effective == Cli::VerdictKind::Found ? "FOUND" :
                           effective == Cli::VerdictKind::Empty ? "EMPTY" :
                           effective == Cli::VerdictKind::Partial ? "PARTIAL" : "NOT VERIFIED";
        nlohmann::json result{{"verdict", kind}, {"count", verdict.Count},
                              {"coverage", {{"read", verdict.Read}, {"total", verdict.Total}}},
                              {"reason", verdict.Kind == Cli::VerdictKind::Empty && effective == Cli::VerdictKind::Partial
                                             ? "coverage incomplete" : verdict.Why},
                              {"output", output}};
        std::printf("%s\n", result.dump().c_str());
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
            PrintVerdict({Cli::VerdictKind::NotVerified, 0, build.error().Format(), 0, 0}, invocation.Json);
            return 2;
        }
        auto cache = DyldSharedCache::Cache::Open(invocation.Cache);
        if (!cache)
        {
            PrintVerdict({Cli::VerdictKind::NotVerified, 0, cache.error().Format(), 0, 0}, invocation.Json);
            return 2;
        }
        auto towers = Cli::ReadTowers(invocation.Towers);
        if (!towers)
        {
            PrintVerdict({Cli::VerdictKind::NotVerified, 0, towers.error().Format(), 0, 0}, invocation.Json);
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
            nlohmann::json result{{"built", report->Done}, {"failed", report->Failed.size()},
                                  {"elapsed_seconds", elapsed}, {"peak_mib", peakMiB},
                                  {"not_verified", notVerified}, {"images", nlohmann::json::array()},
                                  {"failures", nlohmann::json::array()}};
            for (const auto& image : report->Completed)
            {
                result["images"].push_back({{"path", image.Path}, {"bytes", image.Bytes}, {"seconds", image.Seconds},
                    {"peak_mib", image.PeakMiB}, {"coverage", {{"read", image.Decoded}, {"total", image.Total}}}});
            }
            for (const auto& failure : report->Failed)
                result["failures"].push_back({{"path", failure.Path},
                    {"severity", failure.Level == Foundation::Severity::NotVerified ? "NOT VERIFIED" : "FAILED"},
                    {"reason", failure.Reason}});
            std::printf("%s\n", result.dump().c_str());
        }
        else
            std::printf("built %zu image(s), %zu failed, %.1fs, peak %.1f MiB\n", report->Done, report->Failed.size(),
                        elapsed, peakMiB);
        return report->Failed.empty() ? 0 : (notVerified ? 2 : 1);
    }

    int RunBuildDocs(const Cli::Invocation& invocation)
    {
        if (invocation.Repo.empty() || invocation.Documents.empty())
        {
            PrintVerdict({Cli::VerdictKind::NotVerified, 0,
                          "build docs requires --repo and --documents (or their SHERLOCK_* variables)", 0, 0},
                         invocation.Json);
            return 2;
        }
        auto report = DocumentIndex::BuildDocuments(invocation.Repo, invocation.Documents);
        if (!report)
        {
            PrintVerdict({Cli::VerdictKind::NotVerified, 0, report.error().Format(), 0, 0}, invocation.Json);
            return 2;
        }
        std::printf("built %llu section(s), %llu citation(s), %llu seal(s) in %.1fs, head %s\n",
                    static_cast<unsigned long long>(report->SectionsWritten),
                    static_cast<unsigned long long>(report->CitationsWritten),
                    static_cast<unsigned long long>(report->SealsWritten), report->Seconds, report->Head.c_str());
        return 0;
    }
}

int main(int argc, char** argv)
{
    bool requestedJson = false;
    for (int i = 1; i < argc; ++i) requestedJson = requestedJson || std::string_view(argv[i]) == "--json";
    auto invocation = Cli::ParseArguments(argc, argv);
    if (!invocation)
    {
        PrintVerdict({Cli::VerdictKind::NotVerified, 0, invocation.error().Format(), 0, 0}, requestedJson);
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
    if (invocation->Command == "build" && !invocation->Positional.empty() && invocation->Positional.front() == "docs")
    {
        return RunBuildDocs(*invocation);
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

    std::vector<std::string> output;
    Cli::QueryEnvironment env{invocation->Store, invocation->Full, invocation->Json, &output};
    const auto printHeader = [&]() {
        const auto header = Cli::PrintHeader(env, "");
        if (header)
        {
            return true;
        }
        const Cli::Verdict verdict{Cli::VerdictKind::NotVerified, 0, header.error().Format(), 0, 0};
        PrintVerdict(verdict, invocation->Json, output);
        return false;
    };
    if (invocation->Command == "status")
    {
        if (!printHeader())
        {
            return 2;
        }
        const auto verdict = Cli::RunStatus(env);
        const std::string demangler = std::string("demangler: ") +
            (Cli::Demangler::Load(Cli::DefaultDemanglerPath()).has_value() ? "available" : "absent");
        output.push_back(demangler);
        if (!invocation->Json) std::printf("%s\n", demangler.c_str());
        PrintVerdict(verdict, invocation->Json, output);
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
                PrintVerdict({Cli::VerdictKind::NotVerified, 0, "invalid address " + invocation->Positional.front(), 0, 0},
                             invocation->Json, output);
                return 2;
            }
            const auto verdict = Cli::RunCallers(env, *address);
            PrintVerdict(verdict, invocation->Json, output);
            return Cli::ExitCode(verdict);
        }
        auto cache = DyldSharedCache::Cache::Open(invocation->Cache);
        if (!cache)
        {
            PrintVerdict({Cli::VerdictKind::NotVerified, 0, cache.error().Format(), 0, 0}, invocation->Json, output);
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
                PrintVerdict({Cli::VerdictKind::NotVerified, 0, "invalid address " + invocation->Positional.front(), 0, 0},
                             invocation->Json, output);
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
        PrintVerdict(verdict, invocation->Json, output);
        return Cli::ExitCode(verdict);
    }

    PrintVerdict({Cli::VerdictKind::NotVerified, 0,
                  "command " + invocation->Command + " is not implemented in this build", 0, 0}, invocation->Json);
    return 2;
}
