// Sherlock — tools/Sherlock/Source/Facts/include/Facts/Disassembler.h
// One capstone handle, resuming past a word it cannot decode (port of litref.py decode / xisland.py stream).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstdint>
#include <functional>
#include <span>

using csh = std::size_t;

namespace Sherlock::Facts
{
    using Foundation::Expected;

    struct Instruction
    {
        std::uint64_t    Address = 0;
        std::string_view Mnemonic;
        std::string_view Operands;
    };

    struct StreamCoverage
    {
        std::uint64_t Decoded = 0;
        std::uint64_t Total   = 0;
    };

    class Disassembler
    {
    public:
        static Expected<Disassembler> Create();

        Disassembler(Disassembler&& other) noexcept;
        Disassembler& operator=(Disassembler&& other) noexcept;
        Disassembler(const Disassembler&)            = delete;
        Disassembler& operator=(const Disassembler&) = delete;
        ~Disassembler();

        // Every decodable instruction word in `code`; a word capstone refuses is skipped (4 bytes) and the stream
        // resumes, never stops. Coverage is instruction bytes decoded over `code.size()`, matching litref.py.
        StreamCoverage Stream(std::span<const std::byte> code, std::uint64_t base,
                              const std::function<void(const Instruction&)>& onInstruction);

    private:
        explicit Disassembler(csh handle) noexcept : _handle(handle) {}

        csh _handle = 0;
    };
}
