// Sherlock — Source/Facts/VirtualCall.cpp
// SipHash-2-4 discriminator, Itanium signature suffix, and the blraa/braa register tracker, read
// the way References/scripts/vcall.py reads capstone's mnemonic/op_str (derived).
#include <Facts/VirtualCall.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace Sherlock::Facts
{
    using Foundation::DiagnosticCode;

    namespace
    {
        // ------------------------------------------------------------- SipHash-2-4 --
        // c=2 d=4 rounds, 64-bit (non-doubled) output -- the reference algorithm LLVM vendors at
        // llvm/lib/Support/SipHash.cpp, lightly adapted from https://github.com/veorq/SipHash.
        std::uint64_t Rotl(std::uint64_t x, int b)
        {
            return (x << b) | (x >> (64 - b));
        }

        std::uint64_t SipHash24(std::string_view data, std::uint64_t k0, std::uint64_t k1)
        {
            std::uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
            std::uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
            std::uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
            std::uint64_t v3 = 0x7465646279746573ULL ^ k1;

            const auto round = [&]() {
                v0 += v1; v1 = Rotl(v1, 13); v1 ^= v0; v0 = Rotl(v0, 32);
                v2 += v3; v3 = Rotl(v3, 16); v3 ^= v2;
                v0 += v3; v3 = Rotl(v3, 21); v3 ^= v0;
                v2 += v1; v1 = Rotl(v1, 17); v1 ^= v2; v2 = Rotl(v2, 32);
            };

            const std::size_t n   = data.size();
            const std::size_t end = n - (n % 8);
            std::uint64_t     bFinal = static_cast<std::uint64_t>(n & 0xFF) << 56;
            for (std::size_t i = 0; i < end; i += 8)
            {
                std::uint64_t m = 0;
                std::memcpy(&m, data.data() + i, 8);
                v3 ^= m;
                round(); round();
                v0 ^= m;
            }
            for (std::size_t i = end; i < n; ++i)
            {
                bFinal |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i])) << (8 * (i - end));
            }
            v3 ^= bFinal;
            round(); round();
            v0 ^= bFinal;
            v2 ^= 0xff;
            round(); round(); round(); round();
            return v0 ^ v1 ^ v2 ^ v3;
        }

        std::uint64_t LittleEndian64(const std::uint8_t* bytes)
        {
            std::uint64_t v = 0;
            for (int i = 7; i >= 0; --i)
            {
                v = (v << 8) | bytes[i];
            }
            return v;
        }

        // The 16-byte SipHash key Clang's Pointer Authentication ABI specifies as a constant.
        const std::array<std::uint8_t, 16>& SiphashKey()
        {
            static const std::array<std::uint8_t, 16> key = [] {
                constexpr std::string_view          hex = "b5d4c9eb79104a796fec8b1b428781d4";
                std::array<std::uint8_t, 16>         bytes{};
                for (std::size_t i = 0; i < bytes.size(); ++i)
                {
                    unsigned value = 0;
                    std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, value, 16);
                    bytes[i] = static_cast<std::uint8_t>(value);
                }
                return bytes;
            }();
            return key;
        }

        // --------------------------------------------------- local pattern decode --
        bool IsRegister(std::string_view s)
        {
            if (s == "sp" || s == "xzr" || s == "wzr")
            {
                return true;
            }
            if (s.size() < 2 || (s[0] != 'w' && s[0] != 'x'))
            {
                return false;
            }
            int n = 0;
            const auto rest = s.substr(1);
            const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), n);
            return ec == std::errc{} && ptr == rest.data() + rest.size() && n >= 0 && n <= 31;
        }

        std::optional<std::int64_t> ParseImmediate(std::string_view s)
        {
            if (!s.empty() && s.front() == '#')
            {
                s.remove_prefix(1);
            }
            bool negative = false;
            if (!s.empty() && s.front() == '-')
            {
                negative = true;
                s.remove_prefix(1);
            }
            int base = 10;
            if (s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            {
                base = 16;
                s.remove_prefix(2);
            }
            if (s.empty())
            {
                return std::nullopt;
            }
            std::uint64_t magnitude = 0;
            const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), magnitude, base);
            if (ec != std::errc{} || ptr != s.data() + s.size())
            {
                return std::nullopt;
            }
            return negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
        }

        std::vector<std::string_view> SplitOnComma(std::string_view text)
        {
            std::vector<std::string_view> parts;
            std::size_t                   start = 0;
            for (;;)
            {
                const auto comma = text.find(',', start);
                auto       piece = text.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                                       : comma - start);
                while (!piece.empty() && piece.front() == ' ') piece.remove_prefix(1);
                while (!piece.empty() && piece.back() == ' ') piece.remove_suffix(1);
                parts.push_back(piece);
                if (comma == std::string_view::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            return parts;
        }

        // Instructions with no destination register this tracker needs to invalidate, matching
        // vcall.py's _NO_DEST set exactly: stores, compares, every branch, and every PAC
        // authentication mnemonic (whose first operand is READ, not written).
        bool IsNoDestMnemonic(std::string_view m)
        {
            static const std::set<std::string_view> kNoDest = {
                "str", "stur", "sturb", "sturh", "strb", "strh", "stp",
                "cmp", "cmn", "tst", "b", "bl", "blr", "br",
                "blraa", "blraaz", "braa", "braaz", "blrab", "blrabz", "brab", "brabz",
                "ret", "retaa", "retab", "cbz", "cbnz", "tbz", "tbnz", "nop", "prfm",
                "autda", "autdb", "autia", "autib", "xpacd", "xpaci", "brk", "fcmp",
            };
            return kNoDest.contains(m) || m.starts_with("b.");
        }

        // Register-tracking state carried across [windowStart, site+4): the last value LOADED
        // (slotOf), the last movk #imm,lsl#48 target (discOf), an accumulated offset from an
        // authenticated base (addrOf), and a bare mov reg,#imm (constOf) -- vcall.py's four maps,
        // invalidated on any write its own instruction does not recognize as one of the four.
        struct TrackerState
        {
            std::unordered_map<std::string, std::uint64_t> SlotOf, DiscOf, AddrOf, ConstOf;

            void Invalidate(std::string_view reg)
            {
                const std::string key(reg);
                SlotOf.erase(key);
                DiscOf.erase(key);
                AddrOf.erase(key);
                ConstOf.erase(key);
            }
        };

        void TrackInstruction(TrackerState& t, const Instruction& ins)
        {
            const auto parts = SplitOnComma(ins.Operands);

            if (ins.Mnemonic == "movk")
            {
                if (parts.size() == 3 && parts[1].starts_with('#') && parts[2] == "lsl #48")
                {
                    if (const auto imm = ParseImmediate(parts[1]))
                    {
                        const std::string dst(parts[0]);
                        t.DiscOf[dst] = static_cast<std::uint64_t>(*imm);
                        t.SlotOf.erase(dst);
                        return;
                    }
                }
            }
            else if (ins.Mnemonic == "ldr")
            {
                const auto open  = ins.Operands.find('[');
                const auto close = ins.Operands.find(']');
                const auto comma = ins.Operands.find(',');
                if (open != std::string_view::npos && close != std::string_view::npos && comma != std::string_view::npos &&
                    comma < open)
                {
                    std::string dst(ins.Operands.substr(0, comma));
                    while (!dst.empty() && dst.back() == ' ') dst.pop_back();
                    const auto inside      = ins.Operands.substr(open + 1, close - open - 1);
                    const auto insideParts = SplitOnComma(inside);
                    // Only `[base]` or `[base, #imm]` -- vcall.py's _LDR_RE requires the offset
                    // field to start with '#'; a register-offset form (`[base, xN]` or
                    // `[base, xN, lsl #k]`) does not match it at all and falls through to the
                    // generic invalidation below, never a fabricated zero offset.
                    bool          matches = false;
                    std::uint64_t offset  = 0;
                    if (insideParts.size() == 1)
                    {
                        matches = true;
                    }
                    else if (insideParts.size() == 2 && insideParts[1].starts_with('#'))
                    {
                        if (const auto imm = ParseImmediate(insideParts[1]))
                        {
                            offset  = static_cast<std::uint64_t>(*imm);
                            matches = true;
                        }
                    }
                    if (matches)
                    {
                        const std::string base(insideParts[0]);
                        const bool        writeback = close + 1 < ins.Operands.size() && ins.Operands[close + 1] == '!';
                        const auto        baseIt    = t.AddrOf.find(base);
                        const std::uint64_t resolved = (baseIt != t.AddrOf.end() ? baseIt->second : 0) + offset;
                        t.SlotOf[dst] = resolved;
                        t.DiscOf.erase(dst);
                        t.AddrOf.erase(dst);
                        t.ConstOf.erase(dst);
                        if (writeback)
                        {
                            t.AddrOf[base] = resolved;
                        }
                        return;
                    }
                }
            }
            else if (ins.Mnemonic == "add")
            {
                if (parts.size() == 3)
                {
                    const std::string dst(parts[0]), base(parts[1]);
                    const auto        baseIt  = t.AddrOf.find(base);
                    const std::uint64_t baseOffset = baseIt != t.AddrOf.end() ? baseIt->second : 0;
                    if (parts[2].starts_with('#'))
                    {
                        if (const auto imm = ParseImmediate(parts[2]))
                        {
                            t.AddrOf[dst] = baseOffset + static_cast<std::uint64_t>(*imm);
                            t.SlotOf.erase(dst);
                            t.DiscOf.erase(dst);
                            return;
                        }
                    }
                    else if (IsRegister(parts[2]))
                    {
                        const std::string reg(parts[2]);
                        if (const auto constIt = t.ConstOf.find(reg); constIt != t.ConstOf.end())
                        {
                            t.AddrOf[dst] = baseOffset + constIt->second;
                            t.SlotOf.erase(dst);
                            t.DiscOf.erase(dst);
                            return;
                        }
                    }
                }
            }
            else if (ins.Mnemonic == "mov")
            {
                if (parts.size() == 2)
                {
                    const std::string dst(parts[0]);
                    if (parts[1].starts_with('#'))
                    {
                        if (const auto imm = ParseImmediate(parts[1]))
                        {
                            t.ConstOf[dst] = static_cast<std::uint64_t>(*imm);
                            t.AddrOf.erase(dst);
                            t.SlotOf.erase(dst);
                            t.DiscOf.erase(dst);
                            return;
                        }
                    }
                    else if (IsRegister(parts[1]))
                    {
                        const std::string src(parts[1]);
                        t.AddrOf.erase(dst);
                        t.ConstOf.erase(dst);
                        t.SlotOf.erase(dst);
                        t.DiscOf.erase(dst);
                        if (const auto it = t.AddrOf.find(src); it != t.AddrOf.end()) t.AddrOf[dst] = it->second;
                        if (const auto it = t.ConstOf.find(src); it != t.ConstOf.end()) t.ConstOf[dst] = it->second;
                        if (const auto it = t.SlotOf.find(src); it != t.SlotOf.end()) t.SlotOf[dst] = it->second;
                        if (const auto it = t.DiscOf.find(src); it != t.DiscOf.end()) t.DiscOf[dst] = it->second;
                        return;
                    }
                }
            }

            if (!IsNoDestMnemonic(ins.Mnemonic) && !parts.empty() && IsRegister(parts[0]))
            {
                t.Invalidate(parts[0]);
                if (ins.Mnemonic == "ldp" && parts.size() > 1 && IsRegister(parts[1]))
                {
                    t.Invalidate(parts[1]);
                }
            }
        }

        constexpr std::uint64_t kSlotCap = 0x1000;
    }

    std::uint64_t PtrauthStringDiscriminator(std::string_view mangled)
    {
        const auto& key = SiphashKey();
        const std::uint64_t k0 = LittleEndian64(key.data());
        const std::uint64_t k1 = LittleEndian64(key.data() + 8);
        const std::uint64_t hash = SipHash24(mangled, k0, k1);
        return (hash % 0xFFFF) + 1;
    }

    std::string_view StripExportUnderscore(std::string_view exportedName)
    {
        return exportedName.starts_with("__Z") ? exportedName.substr(1) : exportedName;
    }

    std::optional<std::string_view> ItaniumSignatureSuffix(std::string_view name)
    {
        if (!name.starts_with("_ZN"))
        {
            return std::nullopt;
        }
        const std::size_t n = name.size();
        std::size_t       i = 3;
        while (i < n && (name[i] == 'K' || name[i] == 'V' || name[i] == 'r')) ++i;
        std::optional<std::size_t> lastStart;
        while (i < n)
        {
            if (i + 2 <= n)
            {
                const auto two = name.substr(i, 2);
                if (two == "C1" || two == "C2" || two == "C3" || two == "D0" || two == "D1" || two == "D2")
                {
                    lastStart = i;
                    i += 2;
                    break;
                }
            }
            if (std::isdigit(static_cast<unsigned char>(name[i])) != 0)
            {
                std::size_t j = i;
                while (j < n && std::isdigit(static_cast<unsigned char>(name[j])) != 0) ++j;
                std::size_t length = 0;
                const auto digits = name.substr(i, j - i);
                const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), length);
                if (ec != std::errc{} || ptr != digits.data() + digits.size())
                {
                    return std::nullopt;
                }
                lastStart = i;
                i = j + length;
                if (i > n)
                {
                    return std::nullopt;
                }
                if (i < n && name[i] == 'I')
                {
                    return std::nullopt; // template arguments -- not handled, honestly refused
                }
                continue;
            }
            break;
        }
        if (!lastStart || i >= n || name[i] != 'E')
        {
            return std::nullopt;
        }
        return name.substr(*lastStart);
    }

    Expected<std::optional<VirtualCallPattern>> ExtractVirtualCallPatternFromCode(Disassembler& disassembler,
                                                                                  std::span<const std::byte> code,
                                                                                  std::uint64_t windowStart,
                                                                                  std::uint64_t site)
    {
        TrackerState                      tracker;
        std::optional<VirtualCallPattern> result;
        bool                               settled = false;

        const auto coverage = disassembler.Stream(code, windowStart, [&](const Instruction& ins) {
            if (settled)
            {
                return;
            }
            if (ins.Address == site)
            {
                settled = true;
                if (ins.Mnemonic != "blraa" && ins.Mnemonic != "braa")
                {
                    return; // not this pattern -- refused, never guessed
                }
                const auto parts = SplitOnComma(ins.Operands);
                if (parts.size() != 2)
                {
                    return;
                }
                const std::string rn(parts[0]), rm(parts[1]);
                const auto        slotIt = tracker.SlotOf.find(rn);
                const auto        discIt = tracker.DiscOf.find(rm);
                if (slotIt == tracker.SlotOf.end() || discIt == tracker.DiscOf.end())
                {
                    return;
                }
                result = VirtualCallPattern{slotIt->second, discIt->second,
                                            std::string(ins.Mnemonic) + " " + std::string(ins.Operands)};
                return;
            }
            TrackInstruction(tracker, ins);
        });

        if (!coverage)
        {
            return std::unexpected(coverage.error());
        }
        return result;
    }

    Expected<std::optional<VirtualCallPattern>> ExtractVirtualCallPattern(const DyldSharedCache::Cache& cache,
                                                                          Disassembler& disassembler,
                                                                          std::uint64_t windowStart,
                                                                          std::uint64_t site)
    {
        const auto bytes = cache.Read(windowStart, site + 4 - windowStart);
        if (!bytes)
        {
            return std::unexpected(bytes.error());
        }
        return ExtractVirtualCallPatternFromCode(disassembler, *bytes, windowStart, site);
    }

    Expected<std::vector<VirtualCallCandidate>> ResolveVirtualCallFamily(const DyldSharedCache::Cache& cache,
                                                                         const std::vector<MachO::SymbolEntry>& symbols,
                                                                         std::uint64_t slotOffset,
                                                                         std::uint64_t discriminator)
    {
        std::vector<std::uint64_t>                          sortedAddresses;
        std::unordered_map<std::uint64_t, std::string_view> nameAt;
        std::vector<std::pair<std::uint64_t, std::string_view>> ztvs;
        sortedAddresses.reserve(symbols.size());
        for (const auto& symbol : symbols)
        {
            sortedAddresses.push_back(symbol.Address);
            nameAt.try_emplace(symbol.Address, symbol.Name);
            if (symbol.Name.starts_with("__ZTV"))
            {
                ztvs.emplace_back(symbol.Address, symbol.Name);
            }
        }
        std::sort(sortedAddresses.begin(), sortedAddresses.end());
        sortedAddresses.erase(std::unique(sortedAddresses.begin(), sortedAddresses.end()), sortedAddresses.end());
        std::sort(ztvs.begin(), ztvs.end());

        struct Resolved { std::uint64_t Target; std::string_view Name; };
        std::map<std::string_view, Resolved>                    resolvedByZtv;
        std::vector<std::pair<std::uint64_t, std::string_view>> anchors; // (target, ztv name)

        for (const auto& [ztvVa, ztvName] : ztvs)
        {
            const std::uint64_t vptr   = ztvVa + 0x10;
            const std::uint64_t slotVa = vptr + slotOffset;
            const auto           upper = std::upper_bound(sortedAddresses.begin(), sortedAddresses.end(), ztvVa);
            const std::uint64_t  limit = upper != sortedAddresses.end() ? std::min(*upper, vptr + kSlotCap)
                                                                        : vptr + kSlotCap;
            if (slotVa >= limit || !cache.IsMapped(slotVa))
            {
                continue;
            }
            const auto target = cache.DecodePointer(slotVa);
            if (!target)
            {
                if (target.error().Code == DiagnosticCode::Unsupported)
                {
                    return std::unexpected(target.error());
                }
                continue;
            }
            if (!cache.IsMapped(*target))
            {
                continue;
            }
            const auto nameIt = nameAt.find(*target);
            if (nameIt == nameAt.end())
            {
                continue;
            }
            resolvedByZtv.emplace(ztvName, Resolved{*target, nameIt->second});
            if (PtrauthStringDiscriminator(StripExportUnderscore(nameIt->second)) == discriminator)
            {
                anchors.emplace_back(*target, ztvName);
            }
        }

        std::set<std::string_view> suffixes;
        for (const auto& [target, ztvName] : anchors)
        {
            const auto& r = resolvedByZtv.at(ztvName);
            if (const auto suffix = ItaniumSignatureSuffix(StripExportUnderscore(r.Name)))
            {
                suffixes.insert(*suffix);
            }
        }

        std::map<std::uint64_t, std::string_view> family; // target address -> its own name, deduplicated
        for (const auto& [ztvName, r] : resolvedByZtv)
        {
            const auto suffix = ItaniumSignatureSuffix(StripExportUnderscore(r.Name));
            if (suffix && suffixes.contains(*suffix))
            {
                family.emplace(r.Target, r.Name);
            }
        }

        std::vector<VirtualCallCandidate> out;
        out.reserve(family.size());
        for (const auto& [address, name] : family)
        {
            out.push_back({address, std::string(name)});
        }
        return out;
    }
}
