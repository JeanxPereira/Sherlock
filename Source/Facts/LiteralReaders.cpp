// Sherlock — tools/Sherlock/Source/Facts/LiteralReaders.cpp
// Reads capstone's mnemonic/op_str exactly as litref.readers does, so the parity gate compares one rule to one rule.
#include <Facts/LiteralReaders.h>

#include <array>
#include <charconv>
#include <optional>

namespace Sherlock::Facts
{
    namespace
    {
        constexpr std::array<std::string_view, 15> kBarriers = {
            "bl",    "blr",   "ret",   "retaa", "retab", "br",     "braa",   "brab",
            "braaz", "brabz", "blraa", "blrab", "blraaz", "blrabz", "brk",
        };

        bool IsBarrier(std::string_view mnemonic)
        {
            for (const auto barrier : kBarriers)
            {
                if (mnemonic == barrier)
                {
                    return true;
                }
            }
            return false;
        }

        std::optional<std::int64_t> ParseImmediate(std::string_view text)
        {
            while (!text.empty() && text.front() == ' ')
            {
                text.remove_prefix(1);
            }
            if (text.empty() || text.front() != '#')
            {
                return std::nullopt;
            }
            text.remove_prefix(1);
            std::int64_t value   = 0;
            const int     base   = (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? 16 : 10;
            const auto    digits = base == 16 ? text.substr(2) : text;
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
            return ec == std::errc{} && ptr == digits.data() + digits.size() ? std::optional<std::int64_t>(value)
                                                                              : std::nullopt;
        }

        std::optional<std::uint64_t> AddOffset(std::uint64_t base, std::int64_t offset)
        {
            if (offset < 0)
            {
                const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
                return magnitude <= base ? std::optional<std::uint64_t>(base - magnitude) : std::nullopt;
            }
            const auto positive = static_cast<std::uint64_t>(offset);
            return positive <= UINT64_MAX - base ? std::optional<std::uint64_t>(base + positive) : std::nullopt;
        }

        std::vector<std::string_view> SplitOnComma(std::string_view text)
        {
            std::vector<std::string_view> parts;
            std::size_t                   start = 0;
            while (true)
            {
                const auto comma = text.find(',', start);
                auto       piece = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
                while (!piece.empty() && piece.front() == ' ')
                {
                    piece.remove_prefix(1);
                }
                while (!piece.empty() && piece.back() == ' ')
                {
                    piece.remove_suffix(1);
                }
                parts.push_back(piece);
                if (comma == std::string_view::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            return parts;
        }
    }

    void LiteralTracker::Feed(const Instruction& ins, std::vector<LiteralRead>& out)
    {
        const auto mnemonic = ins.Mnemonic;

        if (mnemonic == "adrp")
        {
            const auto comma = ins.Operands.find(',');
            const std::string reg(comma == std::string_view::npos ? ins.Operands : ins.Operands.substr(0, comma));
            const auto value = comma == std::string_view::npos ? std::nullopt
                                                                : ParseImmediate(ins.Operands.substr(comma + 1));
            if (!value)
            {
                _page.erase(reg);
            }
            else
            {
                _page[reg] = static_cast<std::uint64_t>(*value);
            }
            return;
        }

        if (IsBarrier(mnemonic))
        {
            _page.clear();
            return;
        }

        const auto        firstComma  = ins.Operands.find(',');
        const std::string destination = std::string(firstComma == std::string_view::npos ? ins.Operands
                                                                                          : ins.Operands.substr(0, firstComma));

        if (mnemonic == "add")
        {
            const auto parts = SplitOnComma(ins.Operands);
            if (parts.size() == 3)
            {
                const std::string d(parts[0]);
                const std::string n(parts[1]);
                const auto        imm = ParseImmediate(parts[2]);
                const auto        it  = _page.find(n);
                if (it != _page.end() && imm)
                {
                    if (const auto target = AddOffset(it->second, *imm))
                    {
                        out.push_back({ins.Address, *target, "add", d});
                    }
                }
                _page.erase(d);
                return;
            }
        }
        else if (mnemonic == "ldr" || mnemonic == "ldur")
        {
            const auto open  = ins.Operands.find('[');
            const auto close = ins.Operands.find(']');
            if (open != std::string_view::npos && close != std::string_view::npos)
            {
                const auto                    inside = ins.Operands.substr(open + 1, close - open - 1);
                const auto                    parts  = SplitOnComma(inside);
                const std::string              n(parts.empty() ? std::string_view{} : parts[0]);
                const auto                     imm    = parts.size() > 1 ? ParseImmediate(parts[1]) : std::optional<std::int64_t>(0);
                const auto                     it     = _page.find(n);
                if (it != _page.end() && imm)
                {
                    if (const auto target = AddOffset(it->second, *imm))
                    {
                        out.push_back({ins.Address, *target, std::string(mnemonic), destination});
                    }
                }
            }
        }

        _page.erase(destination);
    }
}
