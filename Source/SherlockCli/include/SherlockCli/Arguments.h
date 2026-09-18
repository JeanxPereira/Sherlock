// Sherlock — tools/Sherlock/Source/SherlockCli/include/SherlockCli/Arguments.h
// argv parsed into one Invocation, shared by every subcommand (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Sherlock::Cli
{
    using Foundation::Expected;

    struct Invocation
    {
        std::string                  Command;
        std::vector<std::string>     Positional;
        std::filesystem::path        Cache, Store, Towers, Documents, Repo;
        std::vector<std::string>     Images;
        bool                         AllTowers = false;
        bool                         Full      = false;
        bool                         Json      = false;
        bool                         Resume    = false;
        unsigned                     Workers   = 0;
        std::uint64_t                MinimumFreeBytes = 1024ull * 1024ull * 1024ull;
        std::optional<std::uint64_t> To;
    };

    Expected<Invocation> ParseArguments(int argc, char** argv);
}
