// Sherlock — Source/Foundation/Diagnostic.cpp
// Formatting of a Diagnostic and the Win32 error text it carries.
#include <Foundation/Diagnostic.h>

#include <windows.h>

#include <format>

namespace Sherlock::Foundation
{
    std::string_view Name(DiagnosticCode code) noexcept
    {
        switch (code)
        {
        case DiagnosticCode::NotFound:     return "NotFound";
        case DiagnosticCode::Unmapped:     return "Unmapped";
        case DiagnosticCode::Malformed:    return "Malformed";
        case DiagnosticCode::Unsupported:  return "Unsupported";
        case DiagnosticCode::Io:           return "Io";
        case DiagnosticCode::Database:     return "Database";
        case DiagnosticCode::Disassembler: return "Disassembler";
        case DiagnosticCode::Usage:        return "Usage";
        case DiagnosticCode::Mismatch:     return "Mismatch";
        }
        return "Unknown";
    }

    std::string Diagnostic::Format() const
    {
        std::string text = std::format("{} in {} ({}): {} -- {}", Name(Code), Operation, Subject, Reason, Action);
        if (!System.empty())
        {
            text += std::format(" [system: {}]", System);
        }
        return text;
    }

    std::unexpected<Diagnostic> Fail(DiagnosticCode code, Severity level, std::string operation,
                                     std::string subject, std::string reason, std::string action,
                                     std::string system)
    {
        return std::unexpected(Diagnostic{code, level, std::move(operation), std::move(subject),
                                          std::move(reason), std::move(action), std::move(system)});
    }

    std::string LastSystemError()
    {
        const DWORD code = ::GetLastError();
        char buffer[512] = {};
        const DWORD length = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                              nullptr, code, 0, buffer, sizeof(buffer), nullptr);
        std::string message(buffer, length);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
        {
            message.pop_back();
        }
        return std::format("{}: {}", code, message);
    }

    std::string Hex(std::uint64_t value)
    {
        return std::format("0x{:x}", value);
    }
}
