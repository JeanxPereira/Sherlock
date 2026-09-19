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
        // Layer 2 only: the extracted images IDA opens, and the IDA whose DLLs the worker loads.
        std::filesystem::path        ImagesDir, IdaDir;
        std::vector<std::string>     Images;
        bool                         AllTowers = false;
        bool                         Full      = false;
        bool                         Asm       = false;  // fn only: layer 1's disassembly
        bool                         Json      = false;
        bool                         Resume    = false;
        unsigned                     Workers   = 0;
        std::uint64_t                MinimumFreeBytes = 1024ull * 1024ull * 1024ull;
        // build hexrays only. What bounds layer 2's concurrency is memory: a worker waits for the
        // free physical memory its image predicts, and an image at or over LargeImageBytes runs
        // with no other worker beside it. Zero on either disables that half of the scheduler.
        std::uint64_t                MinimumFreeMemoryBytes = 3ull * 1024 * 1024 * 1024;
        std::uint64_t                LargeImageBytes = 20ull * 1024 * 1024;
        std::optional<std::uint64_t> To;
    };

    Expected<Invocation> ParseArguments(int argc, char** argv);
}
