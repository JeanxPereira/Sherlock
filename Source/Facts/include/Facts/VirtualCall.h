// Sherlock — tools/Sherlock/Source/Facts/include/Facts/VirtualCall.h
// arm64e C++ virtual dispatch: slot+discriminator at a blraa/braa site, and the __ZTV family it
// reaches -- a port of References/scripts/vcall.py's extract_pattern/candidates_for (derived).
#pragma once

#include <DyldSharedCache/Cache.h>
#include <Facts/Disassembler.h>
#include <MachO/Image.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Sherlock::Facts
{
    inline constexpr std::string_view kVirtualCallResolver = "vcall-pac-v1";

    // Clang's ptrauth_string_discriminator: SipHash-2-4 of the mangled name under the constant key
    // https://clang.llvm.org/docs/PointerAuthentication.html specifies, reduced to a non-zero
    // 16-bit value (1..0xFFFF). `mangled` must already be the compiler's Itanium name (one
    // leading underscore) -- strip Apple's exported second underscore first.
    std::uint64_t PtrauthStringDiscriminator(std::string_view mangled);

    // Apple's exported symtab spells every Itanium name with an extra leading underscore
    // (`__ZN...`); the ABI hashes the compiler's own spelling (`_ZN...`).
    std::string_view StripExportUnderscore(std::string_view exportedName);

    // The slice of a `_ZN...E<params>` name from its last nested-name segment (the method name)
    // to the end of the string -- identical for every override of the same virtual, because an
    // overriding declaration repeats the base's name and parameter types exactly. std::nullopt
    // for a shape this simple parser does not recognize (constructor/destructor, template
    // arguments, an ABI tag, anything not `_ZN...E...`) -- never treated as a wildcard match.
    std::optional<std::string_view> ItaniumSignatureSuffix(std::string_view mangled);

    struct VirtualCallPattern
    {
        std::uint64_t SlotOffset    = 0;
        std::uint64_t Discriminator = 0;
        std::string   Instruction; // "blraa x8, x16", exactly as capstone printed it
    };

    // The tracker core, decoupled from the cache so a gate can feed it a synthetic instruction
    // stream directly. `code` is disassembled from `windowStart`; std::nullopt when `site`'s own
    // instruction is not blraa/braa, or the upstream slot-dereferencing ldr / movk discriminator
    // is not found inside the window -- refused, never guessed.
    Expected<std::optional<VirtualCallPattern>> ExtractVirtualCallPatternFromCode(Disassembler& disassembler,
                                                                                  std::span<const std::byte> code,
                                                                                  std::uint64_t windowStart,
                                                                                  std::uint64_t site);

    // (slot, D) at the `blraa`/`braa` VA `site`, tracked from a linear disassembly of
    // `[windowStart, site+4)` read out of `cache` -- a port of vcall.py's extract_pattern.
    Expected<std::optional<VirtualCallPattern>> ExtractVirtualCallPattern(const DyldSharedCache::Cache& cache,
                                                                          Disassembler& disassembler,
                                                                          std::uint64_t windowStart,
                                                                          std::uint64_t site);

    struct VirtualCallCandidate
    {
        std::uint64_t Address = 0;
        std::string   Symbol;  // Apple's own exported name at Address (the `__ZN...` spelling)
    };

    // Every vtable slot among `symbols`'s own `__ZTV...` entries whose target at `slotOffset`
    // hashes to `discriminator` (an anchor -- the introducing declaration or a non-overriding
    // descendant) or shares an anchor's Itanium signature suffix (an override) -- a port of
    // vcall.py's candidates_for. A vtable's own extent is bounded by the next symbol's address
    // (there is no length field): the same overrun a bare mapped-and-is-pointer check cannot see
    // (see vcall.py's module docstring, BLIND SPOTS -- __ZTVN2CA6Render7ContextE+0x68 measured
    // walking into the next symbol's own unrelated data).
    Expected<std::vector<VirtualCallCandidate>> ResolveVirtualCallFamily(const DyldSharedCache::Cache& cache,
                                                                         const std::vector<MachO::SymbolEntry>& symbols,
                                                                         std::uint64_t slotOffset,
                                                                         std::uint64_t discriminator);
}
