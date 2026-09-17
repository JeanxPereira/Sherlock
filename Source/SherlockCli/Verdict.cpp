// Sherlock — tools/Sherlock/Source/SherlockCli/Verdict.cpp
// verdict: FOUND/EMPTY/PARTIAL/NOT VERIFIED, and the EMPTY -> PARTIAL coverage-incomplete downgrade.
#include <SherlockCli/Verdict.h>

#include <format>

namespace Sherlock::Cli
{
    namespace
    {
        bool Incomplete(const Verdict& verdict)
        {
            return verdict.Kind == VerdictKind::Empty && verdict.Read != verdict.Total;
        }
    }

    std::string FormatVerdict(const Verdict& verdict)
    {
        if (verdict.Kind == VerdictKind::NotVerified)
        {
            return "verdict: NOT VERIFIED " + verdict.Why;
        }
        if (Incomplete(verdict))
        {
            return std::format("verdict: PARTIAL coverage incomplete  coverage {}/{}", verdict.Read, verdict.Total);
        }
        if (verdict.Kind == VerdictKind::Partial)
        {
            return std::format("verdict: PARTIAL {}  coverage {}/{}", verdict.Why, verdict.Read, verdict.Total);
        }
        if (verdict.Kind == VerdictKind::Empty)
        {
            return std::format("verdict: EMPTY  coverage {}/{}", verdict.Read, verdict.Total);
        }
        return std::format("verdict: FOUND {}  coverage {}/{}", verdict.Count, verdict.Read, verdict.Total);
    }

    int ExitCode(const Verdict& verdict)
    {
        if (verdict.Kind == VerdictKind::NotVerified)
        {
            return 2;
        }
        if (Incomplete(verdict) || verdict.Kind == VerdictKind::Partial)
        {
            return 3;
        }
        return 0;
    }
}
