// Sherlock — tools/Sherlock/Source/Facts/Disassembler.cpp
// capstone lifetime and cs_disasm_iter behind Disassembler::Stream.
#include <Facts/Disassembler.h>

#include <capstone/capstone.h>

#include <utility>
#include <memory>

namespace Sherlock::Facts
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    Expected<Disassembler> Disassembler::Create()
    {
        csh handle = 0;
        if (::cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) != CS_ERR_OK)
        {
            return Fail(DiagnosticCode::Disassembler, Severity::NotVerified, "Disassembler::Create", "capstone",
                        "cs_open failed", "check the capstone build");
        }
        ::cs_option(handle, CS_OPT_DETAIL, CS_OPT_OFF);
        return Disassembler(handle);
    }

    Disassembler::Disassembler(Disassembler&& other) noexcept : _handle(std::exchange(other._handle, 0))
    {
    }

    Disassembler& Disassembler::operator=(Disassembler&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle != 0)
            {
                ::cs_close(&_handle);
            }
            _handle = std::exchange(other._handle, 0);
        }
        return *this;
    }

    Disassembler::~Disassembler()
    {
        if (_handle != 0)
        {
            ::cs_close(&_handle);
        }
    }

    Expected<StreamCoverage> Disassembler::Stream(std::span<const std::byte> code, std::uint64_t base,
                                                  const std::function<void(const Instruction&)>& onInstruction)
    {
        StreamCoverage coverage{0, code.size() / 4};
        const auto freeInstruction = [](cs_insn* value) { if (value != nullptr) ::cs_free(value, 1); };
        std::unique_ptr<cs_insn, decltype(freeInstruction)> insn(::cs_malloc(_handle), freeInstruction);
        if (!insn)
        {
            return Fail(DiagnosticCode::Disassembler, Severity::NotVerified, "Disassembler::Stream", "capstone",
                        "cs_malloc failed", "free memory and retry");
        }
        const auto*    cursor  = reinterpret_cast<const std::uint8_t*>(code.data());
        std::size_t    left    = code.size();
        std::uint64_t  address = base;

        while (left > 0)
        {
            if (::cs_disasm_iter(_handle, &cursor, &left, &address, insn.get()))
            {
                coverage.Decoded += insn->size / 4;
                onInstruction(Instruction{insn->address, std::string_view(insn->mnemonic), std::string_view(insn->op_str)});
            }
            else if (left >= 4)
            {
                cursor  += 4;
                address += 4;
                left    -= 4;
            }
            else
            {
                break;
            }
        }
        return coverage;
    }
}
