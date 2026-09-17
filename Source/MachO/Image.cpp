// Sherlock — tools/Sherlock/Source/MachO/Image.cpp
// Load-command walk of one Mach-O 64 image: segments, sections, LC_SYMTAB, LC_FUNCTION_STARTS.
#include <MachO/Image.h>

#include <Foundation/ByteReader.h>

#include <cstring>

namespace Sherlock::MachO
{
    using Foundation::ByteReader;
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Hex;
    using Foundation::Severity;

    namespace
    {
        struct MachHeader64
        {
            std::uint32_t Magic, CpuType, CpuSubtype, FileType, NCmds, SizeOfCmds, Flags, Reserved;
        };
        static_assert(sizeof(MachHeader64) == 32);

        struct LoadCommand
        {
            std::uint32_t Cmd, CmdSize;
        };
        static_assert(sizeof(LoadCommand) == 8);

        struct Segment64Command
        {
            std::uint32_t Cmd, CmdSize;
            char          SegName[16];
            std::uint64_t VmAddr, VmSize, FileOff, FileSize;
            std::uint32_t MaxProt, InitProt, NSects, Flags;
        };
        static_assert(sizeof(Segment64Command) == 72 && offsetof(Segment64Command, VmAddr) == 24 &&
                      offsetof(Segment64Command, NSects) == 64);

        struct Section64
        {
            char          SectName[16], SegName[16];
            std::uint64_t Addr, Size;
            std::uint32_t Offset, Align, RelOff, NReloc, Flags, Reserved1, Reserved2, Reserved3;
        };
        static_assert(sizeof(Section64) == 80 && offsetof(Section64, Addr) == 32 && offsetof(Section64, Size) == 40);

        struct SymtabCommand
        {
            std::uint32_t Cmd, CmdSize, SymOff, NSyms, StrOff, StrSize;
        };
        static_assert(sizeof(SymtabCommand) == 24 && offsetof(SymtabCommand, SymOff) == 8);

        struct FunctionStartsCommand
        {
            std::uint32_t Cmd, CmdSize, DataOff, DataSize;
        };
        static_assert(sizeof(FunctionStartsCommand) == 16 && offsetof(FunctionStartsCommand, DataOff) == 8);

        struct NList64
        {
            std::uint32_t NStrx;
            std::uint8_t  NType;
            std::uint8_t  NSect;
            std::uint16_t NDesc;
            std::uint64_t NValue;
        };
        static_assert(sizeof(NList64) == 16 && offsetof(NList64, NValue) == 8);

        constexpr std::uint32_t kMagic64          = 0xFEEDFACFu;
        constexpr std::uint32_t kLcSegment64      = 0x19;
        constexpr std::uint32_t kLcSymtab         = 0x2;
        constexpr std::uint32_t kLcFunctionStarts = 0x26;

        std::string FixedName(const char* bytes, std::size_t length)
        {
            return std::string(bytes, ::strnlen(bytes, length));
        }
    }

    std::string Section::FullName() const
    {
        return Segment + "." + Name;
    }

    const Section* Image::FindSection(std::string_view segment, std::string_view section) const noexcept
    {
        for (const auto& s : _sections)
        {
            if (s.Segment == segment && s.Name == section)
            {
                return &s;
            }
        }
        return nullptr;
    }

    Expected<Image> Image::Parse(const Foundation::AddressSpace& space, std::uint64_t header)
    {
        const auto headerBytes = space.Read(header, sizeof(MachHeader64));
        if (!headerBytes)
        {
            return std::unexpected(headerBytes.error());
        }
        const auto mach = ByteReader(*headerBytes).At<MachHeader64>(0);
        if (!mach)
        {
            return std::unexpected(mach.error());
        }
        if (mach->Magic != kMagic64)
        {
            return Fail(DiagnosticCode::Unsupported, Severity::NotVerified, "Image::Parse", Hex(header),
                        "the header does not carry the 64-bit Mach-O magic", "point at a Mach-O 64 header");
        }

        Image image;
        image._header = header;

        std::uint64_t cursor = header + sizeof(MachHeader64);
        for (std::uint32_t i = 0; i < mach->NCmds; ++i)
        {
            const auto lcBytes = space.Read(cursor, sizeof(LoadCommand));
            if (!lcBytes)
            {
                return std::unexpected(lcBytes.error());
            }
            const auto lc = ByteReader(*lcBytes).At<LoadCommand>(0);
            if (!lc)
            {
                return std::unexpected(lc.error());
            }
            if (lc->CmdSize < sizeof(LoadCommand))
            {
                return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Image::Parse", Hex(cursor),
                            "a load command's size is smaller than its own header", "re-read the cache");
            }

            if (lc->Cmd == kLcSegment64)
            {
                const auto segBytes = space.Read(cursor, sizeof(Segment64Command));
                if (!segBytes)
                {
                    return std::unexpected(segBytes.error());
                }
                const auto seg = ByteReader(*segBytes).At<Segment64Command>(0);
                if (!seg)
                {
                    return std::unexpected(seg.error());
                }
                const std::string segName = FixedName(seg->SegName, sizeof(seg->SegName));
                image._segments.push_back({segName, seg->VmAddr, seg->VmSize});
                if (segName == "__LINKEDIT")
                {
                    image._hasLinkedit     = true;
                    image._linkeditVa      = seg->VmAddr;
                    image._linkeditFileoff = seg->FileOff;
                }

                for (std::uint32_t s = 0; s < seg->NSects; ++s)
                {
                    const std::uint64_t sectionVa = cursor + sizeof(Segment64Command) +
                                                    static_cast<std::uint64_t>(s) * sizeof(Section64);
                    const auto secBytes = space.Read(sectionVa, sizeof(Section64));
                    if (!secBytes)
                    {
                        return std::unexpected(secBytes.error());
                    }
                    const auto sec = ByteReader(*secBytes).At<Section64>(0);
                    if (!sec)
                    {
                        return std::unexpected(sec.error());
                    }
                    image._sections.push_back({FixedName(sec->SegName, sizeof(sec->SegName)),
                                               FixedName(sec->SectName, sizeof(sec->SectName)), sec->Addr, sec->Size});
                }
            }
            else if (lc->Cmd == kLcSymtab)
            {
                const auto symBytes = space.Read(cursor, sizeof(SymtabCommand));
                if (!symBytes)
                {
                    return std::unexpected(symBytes.error());
                }
                const auto sym = ByteReader(*symBytes).At<SymtabCommand>(0);
                if (!sym)
                {
                    return std::unexpected(sym.error());
                }
                image._hasSymtab = true;
                image._symoff    = sym->SymOff;
                image._nsyms     = sym->NSyms;
                image._stroff    = sym->StrOff;
                image._strsize   = sym->StrSize;
            }
            else if (lc->Cmd == kLcFunctionStarts)
            {
                const auto fsBytes = space.Read(cursor, sizeof(FunctionStartsCommand));
                if (!fsBytes)
                {
                    return std::unexpected(fsBytes.error());
                }
                const auto fs = ByteReader(*fsBytes).At<FunctionStartsCommand>(0);
                if (!fs)
                {
                    return std::unexpected(fs.error());
                }
                image._hasFunctionStarts  = true;
                image._functionStartsOff  = fs->DataOff;
                image._functionStartsSize = fs->DataSize;
            }

            cursor += lc->CmdSize;
        }
        return image;
    }

    Expected<std::uint64_t> Image::LinkeditFileToVa(std::uint64_t fileOffset) const
    {
        if (!_hasLinkedit)
        {
            return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Image::LinkeditFileToVa", Hex(_header),
                        "the image carries no __LINKEDIT segment", "check the image's load commands");
        }
        if (fileOffset < _linkeditFileoff)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Image::LinkeditFileToVa", Hex(fileOffset),
                        "the offset falls before __LINKEDIT's own file offset", "check the load command that produced it");
        }
        return _linkeditVa + (fileOffset - _linkeditFileoff);
    }

    Expected<std::vector<std::uint64_t>> Image::FunctionStarts(const Foundation::AddressSpace& space) const
    {
        if (!_hasFunctionStarts)
        {
            return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Image::FunctionStarts", Hex(_header),
                        "the image carries no LC_FUNCTION_STARTS", "the image predates function starts; use symbols instead");
        }
        const auto va = LinkeditFileToVa(_functionStartsOff);
        if (!va)
        {
            return std::unexpected(va.error());
        }
        const auto data = space.Read(*va, _functionStartsSize);
        if (!data)
        {
            return std::unexpected(data.error());
        }

        const Segment* text = nullptr;
        for (const auto& segment : _segments)
        {
            if (segment.Name == "__TEXT")
            {
                text = &segment;
                break;
            }
        }
        if (text == nullptr)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Image::FunctionStarts", Hex(_header),
                        "the image carries no __TEXT segment", "check the image's load commands");
        }

        std::vector<std::uint64_t> starts;
        std::uint64_t              address = text->Address;
        std::size_t                pos     = 0;
        while (pos < data->size())
        {
            std::uint64_t delta = 0;
            int           shift = 0;
            bool          more  = true;
            while (more && pos < data->size())
            {
                const auto byte = static_cast<std::uint8_t>((*data)[pos++]);
                delta |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
                shift += 7;
                more = (byte & 0x80) != 0;
            }
            if (delta == 0)
            {
                break;
            }
            address += delta;
            starts.push_back(address);
        }
        return starts;
    }

    Expected<std::vector<SymbolEntry>> Image::Symbols(const Foundation::AddressSpace& space) const
    {
        if (!_hasSymtab)
        {
            return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Image::Symbols", Hex(_header),
                        "the image carries no LC_SYMTAB", "check the image's load commands");
        }
        const auto symVa = LinkeditFileToVa(_symoff);
        if (!symVa)
        {
            return std::unexpected(symVa.error());
        }
        const auto strVa = LinkeditFileToVa(_stroff);
        if (!strVa)
        {
            return std::unexpected(strVa.error());
        }
        const auto symbolBytes = space.Read(*symVa, static_cast<std::size_t>(_nsyms) * sizeof(NList64));
        if (!symbolBytes)
        {
            return std::unexpected(symbolBytes.error());
        }
        const auto stringBytes = space.Read(*strVa, _strsize);
        if (!stringBytes)
        {
            return std::unexpected(stringBytes.error());
        }
        const ByteReader table(*symbolBytes);
        const ByteReader strings(*stringBytes);

        std::vector<SymbolEntry> symbols;
        symbols.reserve(_nsyms);
        for (std::uint32_t i = 0; i < _nsyms; ++i)
        {
            const auto entry = table.At<NList64>(static_cast<std::size_t>(i) * sizeof(NList64));
            if (!entry)
            {
                return std::unexpected(entry.error());
            }
            if ((entry->NType & 0xE0) != 0 || (entry->NType & 0x0E) != 0x0E || entry->NValue == 0)
            {
                continue;
            }
            if (entry->NStrx >= _strsize)
            {
                continue;
            }
            const auto name = strings.CString(entry->NStrx, _strsize - entry->NStrx);
            if (!name)
            {
                continue;
            }
            symbols.push_back({entry->NValue, std::string(*name), (entry->NType & 0x01) != 0});
        }
        return symbols;
    }
}
