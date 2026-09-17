// Sherlock — tools/Sherlock/Source/Facts/include/Facts/BranchIsland.h
// island -> target, a hop-limited port of xisland.py's island_target (derived).
#pragma once

#include <DyldSharedCache/Cache.h>
#include <Facts/Disassembler.h>

#include <optional>

namespace Sherlock::Facts
{
    inline constexpr int kMaxIslandHops = 8;

    Expected<std::optional<std::uint64_t>> ResolveIsland(const DyldSharedCache::Cache& cache, Disassembler& disassembler,
                                                          std::uint64_t island);
}
