// Sherlock — tools/Sherlock/Source/Facts/BranchIsland.cpp
// adrp/add/ldr slot decoding and plain-b chaining, read the way xisland.py's island_target reads capstone's text.
#include <Facts/BranchIsland.h>

#include <set>

namespace Sherlock::Facts
{
    using Foundation::DiagnosticCode;
    using Foundation::Severity;

    namespace
    {
        std::uint64_t ParseHexOperand(std::string_view text)
        {
            std::size_t start = 0;
            while (start < text.size() && (text[start] == ' ' || text[start] == '#'))
            {
                ++start;
            }
            return std::stoull(std::string(text.substr(start)), nullptr, 16);
        }

        Expected<std::optional<std::uint64_t>> OneIsland(const DyldSharedCache::Cache& cache, Disassembler& disassembler,
                                                          std::uint64_t va)
        {
            const auto bytes = cache.Read(va, 32);
            if (!bytes)
            {
                return std::nullopt;
            }

            std::optional<std::uint64_t>   page;
            std::optional<std::uint64_t>   plainBranch;
            std::optional<Foundation::Diagnostic> propagate;
            bool                            terminal = false;

            const auto coverage = disassembler.Stream(*bytes, va, [&](const Instruction& ins) {
                if (page && !plainBranch && !terminal)
                {
                    // adrp already matched; the remaining branches below only fire once.
                }
                if (ins.Mnemonic == "adrp")
                {
                    const auto comma = ins.Operands.find(", ");
                    page = comma == std::string_view::npos ? std::optional<std::uint64_t>{}
                                                            : ParseHexOperand(ins.Operands.substr(comma + 2));
                }
                else if (ins.Mnemonic == "add" && page)
                {
                    const auto lastComma = ins.Operands.rfind(',');
                    if (lastComma != std::string_view::npos)
                    {
                        *page += ParseHexOperand(ins.Operands.substr(lastComma + 1));
                    }
                }
                else if (ins.Mnemonic == "ldr" && page && !plainBranch && !terminal)
                {
                    const auto open  = ins.Operands.find('[');
                    const auto close = ins.Operands.find(']');
                    if (open != std::string_view::npos && close != std::string_view::npos)
                    {
                        const std::string_view inside = ins.Operands.substr(open + 1, close - open - 1);
                        const auto             sep     = inside.find(", #");
                        const std::uint64_t     offset  = sep == std::string_view::npos ? 0 : ParseHexOperand(inside.substr(sep + 2));
                        const auto              decoded = cache.DecodePointer(*page + offset);
                        if (decoded)
                        {
                            plainBranch = *decoded;
                        }
                        else if (decoded.error().Code == DiagnosticCode::Unsupported)
                        {
                            propagate = decoded.error();
                        }
                        terminal = true;
                    }
                }
                else if (ins.Mnemonic == "b" && !plainBranch && !terminal)
                {
                    plainBranch = ParseHexOperand(ins.Operands);
                    terminal    = true;
                }
                else if ((ins.Mnemonic == "br" || ins.Mnemonic == "braa" || ins.Mnemonic == "brab" ||
                          ins.Mnemonic == "blr" || ins.Mnemonic == "ret") && !terminal)
                {
                    terminal = true;
                }
            });

            if (!coverage)
            {
                return std::unexpected(coverage.error());
            }

            if (propagate)
            {
                return std::unexpected(*propagate);
            }
            return plainBranch;
        }
    }

    Expected<std::optional<std::uint64_t>> ResolveIsland(const DyldSharedCache::Cache& cache, Disassembler& disassembler,
                                                          std::uint64_t island)
    {
        std::set<std::uint64_t> visited;
        std::uint64_t           current = island;
        for (int hop = 0; hop < kMaxIslandHops; ++hop)
        {
            if (!visited.insert(current).second)
            {
                return std::nullopt;
            }
            const auto target = OneIsland(cache, disassembler, current);
            if (!target)
            {
                return std::unexpected(target.error());
            }
            if (!*target)
            {
                return std::nullopt;
            }
            if (cache.Owner(**target).has_value())
            {
                return **target;
            }
            current = **target;
        }
        return std::nullopt;
    }
}
