// Sherlock — tools/Sherlock/Source/Foundation/include/Foundation/AddressSpace.h
// Read-by-virtual-address, the one thing a Mach-O parser needs from its host (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Sherlock::Foundation
{
    class AddressSpace
    {
    public:
        virtual ~AddressSpace() = default;

        // Exactly `size` bytes at `va`, or Unmapped when any of them is not mapped.
        virtual Expected<std::span<const std::byte>> Read(std::uint64_t va, std::size_t size) const = 0;
    };
}
