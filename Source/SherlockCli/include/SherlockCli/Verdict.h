// Sherlock — tools/Sherlock/Source/SherlockCli/include/SherlockCli/Verdict.h
// The verdict line and its exit code, byte-exact with References/scripts/verdict.py (derived).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Sherlock::Cli
{
    enum class VerdictKind
    {
        Found,
        Empty,
        Partial,
        NotVerified,
    };

    struct Verdict
    {
        VerdictKind   Kind = VerdictKind::NotVerified;
        std::size_t   Count = 0;
        std::string   Why;
        std::uint64_t Read = 0, Total = 0;
    };

    std::string FormatVerdict(const Verdict& verdict);
    int         ExitCode(const Verdict& verdict);
}
