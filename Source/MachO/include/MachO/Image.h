// Sherlock — Source/MachO/include/MachO/Image.h
// Segments, sections, function starts and the symbol table of one Mach-O 64 image (Apple name).
#pragma once

#include <Foundation/AddressSpace.h>
#include <Foundation/Diagnostic.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Sherlock::MachO
{
    using Foundation::Expected;

    struct Segment
    {
        std::string   Name;
        std::uint64_t Address = 0;
        std::uint64_t Size    = 0;
    };

    struct Section
    {
        std::string   Segment;
        std::string   Name;
        std::uint64_t Address = 0;
        std::uint64_t Size    = 0;

        // "__TEXT.__text"
        std::string FullName() const;
    };

    struct SymbolEntry
    {
        std::uint64_t Address  = 0;
        std::string   Name;
        bool          External = false;
    };

    class Image
    {
    public:
        static Expected<Image> Parse(const Foundation::AddressSpace& space, std::uint64_t header);

        const std::vector<Segment>& Segments() const noexcept { return _segments; }
        const std::vector<Section>& Sections() const noexcept { return _sections; }
        const Section* FindSection(std::string_view segment, std::string_view section) const noexcept;

        Expected<std::vector<std::uint64_t>> FunctionStarts(const Foundation::AddressSpace& space) const;
        Expected<std::vector<SymbolEntry>>   Symbols(const Foundation::AddressSpace& space) const;

    private:
        Image() = default;

        Expected<std::uint64_t> LinkeditFileToVa(std::uint64_t fileOffset) const;

        std::vector<Segment> _segments;
        std::vector<Section> _sections;
        std::uint64_t        _header = 0;

        bool          _hasLinkedit     = false;
        std::uint64_t _linkeditVa      = 0;
        std::uint64_t _linkeditFileoff = 0;

        bool          _hasSymtab = false;
        std::uint32_t _symoff    = 0;
        std::uint32_t _nsyms     = 0;
        std::uint32_t _stroff    = 0;
        std::uint32_t _strsize   = 0;

        bool          _hasFunctionStarts  = false;
        std::uint32_t _functionStartsOff  = 0;
        std::uint32_t _functionStartsSize = 0;
    };
}
