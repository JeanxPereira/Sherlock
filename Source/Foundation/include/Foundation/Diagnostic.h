// Sherlock — Source/Foundation/include/Foundation/Diagnostic.h
// A failure as a value: what failed, on what, why, and the one action that fixes it (derived).
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace Sherlock::Foundation
{
    enum class DiagnosticCode : std::uint8_t
    {
        NotFound,
        Unmapped,
        Malformed,
        Unsupported,
        Io,
        Database,
        Disassembler,
        Usage,
        Mismatch,
    };

    // Failed: the instrument looked and the subject is wrong (exit 1).
    // NotVerified: the instrument could not look (exit 2).
    enum class Severity : std::uint8_t
    {
        Failed,
        NotVerified,
    };

    struct Diagnostic
    {
        DiagnosticCode Code  = DiagnosticCode::Malformed;
        Severity       Level = Severity::NotVerified;
        std::string    Operation;
        std::string    Subject;
        std::string    Reason;
        std::string    Action;
        std::string    System;

        // "<code> in <operation> (<subject>): <reason> -- <action>", then " [system: <system>]" when set.
        std::string Format() const;
    };

    template <class T>
    using Expected = std::expected<T, Diagnostic>;

    std::string_view Name(DiagnosticCode code) noexcept;

    std::unexpected<Diagnostic> Fail(DiagnosticCode code, Severity level, std::string operation,
                                     std::string subject, std::string reason, std::string action,
                                     std::string system = {});

    std::string LastSystemError();

    std::string Hex(std::uint64_t value);
}
