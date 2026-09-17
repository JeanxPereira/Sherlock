// Sherlock — tools/Sherlock/Source/Facts/include/Facts/LiteralReaders.h
// The adrp + add/ldr literal-address tracker, a port of litref.py's readers() (derived).
#pragma once

#include <Facts/Disassembler.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace Sherlock::Facts
{
    struct LiteralRead
    {
        std::uint64_t Site        = 0;
        std::uint64_t Target      = 0;
        std::string   Kind;
        std::string   Destination;
    };

    // Register names die on a call or a return, matching litref.py's BARRIERS tuple exactly.
    class LiteralTracker
    {
    public:
        void Feed(const Instruction& instruction, std::vector<LiteralRead>& out);

    private:
        std::unordered_map<std::string, std::uint64_t> _page;
    };
}
