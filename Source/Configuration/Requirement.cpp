// Sherlock — Source/Configuration/Requirement.cpp
// The "sherlock" key: which Sherlock versions a consumer's sherlock.json accepts.
#include <Configuration/Config.h>

#include <charconv>
#include <format>
#include <optional>

namespace Sherlock::Configuration
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        struct Version
        {
            unsigned Major = 0, Minor = 0, Patch = 0;
        };

        std::optional<Version> ParseVersion(std::string_view text, bool patchOptional)
        {
            Version     version;
            unsigned*   parts[] = {&version.Major, &version.Minor, &version.Patch};
            std::size_t count   = 0;
            for (;;)
            {
                if (count == 3) return std::nullopt;
                const auto dot   = text.find('.');
                const auto piece = text.substr(0, dot);
                if (piece.empty()) return std::nullopt;
                const auto [end, error] = std::from_chars(piece.data(), piece.data() + piece.size(), *parts[count]);
                if (error != std::errc{} || end != piece.data() + piece.size()) return std::nullopt;
                ++count;
                if (dot == std::string_view::npos) break;
                text.remove_prefix(dot + 1);
            }
            if (count == 3 || (count == 2 && patchOptional)) return version;
            return std::nullopt;
        }
    }

    Foundation::Expected<void> CheckRequirement(std::string_view required, std::string_view running)
    {
        const auto want = ParseVersion(required, true);
        if (!want)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Configuration::CheckRequirement",
                        std::string(required),
                        std::format("\"sherlock\" is \"{}\", not MAJOR.MINOR or MAJOR.MINOR.PATCH", required),
                        "write the required version, e.g. \"0.2\"");
        }
        const auto have = ParseVersion(running, false);
        if (!have)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Configuration::CheckRequirement",
                        std::string(running), std::format("this Sherlock's own version \"{}\" is not MAJOR.MINOR.PATCH", running),
                        "rebuild Sherlock");
        }
        const bool accepted = want->Major == 0
            ? have->Major == 0 && have->Minor == want->Minor && have->Patch >= want->Patch
            : have->Major == want->Major &&
                  (have->Minor > want->Minor || (have->Minor == want->Minor && have->Patch >= want->Patch));
        if (accepted) return {};
        return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "Configuration::CheckRequirement",
                    std::string(required),
                    std::format("sherlock.json requires Sherlock {} and this is Sherlock {}", required, running),
                    std::format("install a Sherlock {}.{}.x, or change the \"sherlock\" requirement", want->Major, want->Minor));
    }
}
