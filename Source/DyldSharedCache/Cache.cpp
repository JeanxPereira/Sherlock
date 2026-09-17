// Sherlock — tools/Sherlock/Source/DyldSharedCache/Cache.cpp
// Opening, addressing and pointer decoding of the split dyld shared cache.
#include <DyldSharedCache/Cache.h>
#include <DyldSharedCache/Format.h>

#include <Foundation/ByteReader.h>

#include <algorithm>
#include <format>

namespace Sherlock::DyldSharedCache
{
    using Foundation::ByteReader;
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Hex;
    using Foundation::Severity;

    namespace
    {
        std::string UuidText(std::span<const std::byte> bytes)
        {
            std::string text;
            for (const std::byte b : bytes)
            {
                text += std::format("{:02x}", static_cast<unsigned>(b));
            }
            return text;
        }

        Expected<std::filesystem::path> MainFile(const std::filesystem::path& directory)
        {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(directory, error))
            {
                const std::string name = entry.path().filename().string();
                constexpr std::string_view prefix = "dyld_shared_cache_";
                if (entry.is_regular_file() && name.starts_with(prefix) &&
                    name.find('.', prefix.size()) == std::string::npos)
                {
                    return entry.path();
                }
            }
            return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Cache::Open", directory.string(),
                        "no dyld_shared_cache_<arch> main file in the directory",
                        "point SHERLOCK_CACHE at the extracted cache directory",
                        error ? error.message() : std::string{});
        }
    }

    Expected<void> Cache::AddFile(Foundation::MappedFile file)
    {
        const ByteReader header(file.Bytes());
        const std::size_t index = _files.size();
        const auto mappingOffset = header.At<std::uint32_t>(Format::kMappingOffset);
        const auto mappingCount  = header.At<std::uint32_t>(Format::kMappingCount);
        const auto slideOffset   = header.At<std::uint32_t>(Format::kMappingWithSlideOffset);
        const auto slideCount    = header.At<std::uint32_t>(Format::kMappingWithSlideCount);
        if (!mappingOffset || !mappingCount || !slideOffset || !slideCount)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Cache::AddFile", file.Path().string(),
                        "the header is shorter than the mapping tables it names", "re-extract the cache");
        }

        const bool withSlide = *slideOffset != 0 && *slideCount == *mappingCount;
        for (std::uint32_t i = 0; i < *mappingCount; ++i)
        {
            Mapping mapping;
            mapping.File = index;
            if (withSlide)
            {
                const auto info = header.At<Format::MappingAndSlideInfo>(*slideOffset + i * sizeof(Format::MappingAndSlideInfo));
                if (!info)
                {
                    return std::unexpected(info.error());
                }
                mapping.Address    = info->Address;
                mapping.Size       = info->Size;
                mapping.FileOffset = info->FileOffset;
                if (info->SlideInfoFileSize != 0)
                {
                    const auto slide = header.At<Format::SlideInfo5>(info->SlideInfoFileOffset);
                    if (!slide)
                    {
                        return std::unexpected(slide.error());
                    }
                    mapping.SlideInfo = Slide{slide->Version, slide->PageSize, slide->ValueAdd, index,
                                              static_cast<std::size_t>(info->SlideInfoFileOffset + sizeof(Format::SlideInfo5)),
                                              slide->PageStartsCount};
                }
            }
            else
            {
                const auto info = header.At<Format::MappingInfo>(*mappingOffset + i * sizeof(Format::MappingInfo));
                if (!info)
                {
                    return std::unexpected(info.error());
                }
                mapping.Address    = info->Address;
                mapping.Size       = info->Size;
                mapping.FileOffset = info->FileOffset;
            }
            if (mapping.Size == 0)
            {
                continue;
            }
            if (mapping.FileOffset > file.Bytes().size() || file.Bytes().size() - mapping.FileOffset < mapping.Size)
            {
                return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Cache::AddFile", file.Path().string(),
                            std::format("mapping at {} runs past the end of the file", Hex(mapping.Address)),
                            "re-extract the cache");
            }
            _mappings.push_back(mapping);
        }
        _files.push_back(std::move(file));
        return {};
    }

    Expected<Cache> Cache::Open(const std::filesystem::path& directory)
    {
        const auto mainPath = MainFile(directory);
        if (!mainPath)
        {
            return std::unexpected(mainPath.error());
        }
        auto main = Foundation::MappedFile::Open(*mainPath);
        if (!main)
        {
            return std::unexpected(main.error());
        }

        const ByteReader header(main->Bytes());
        const auto magic = header.CString(Format::kMagic, 16);
        if (!magic || !magic->starts_with("dyld_v1"))
        {
            return Fail(DiagnosticCode::Unsupported, Severity::NotVerified, "Cache::Open", mainPath->string(),
                        "the file does not carry the dyld_v1 magic", "point at a dyld shared cache");
        }
        const auto mappingOffset = header.At<std::uint32_t>(Format::kMappingOffset);
        if (!mappingOffset || *mappingOffset <= Format::kCacheSubTypeEnd)
        {
            return Fail(DiagnosticCode::Unsupported, Severity::NotVerified, "Cache::Open", mainPath->string(),
                        "the header predates suffixed subcache entries (subcache entry form v1)",
                        "add support for this cache generation before reading it");
        }

        Cache cache;
        cache._uuid = UuidText(main->Bytes().subspan(Format::kUuid, 16));
        const auto subOffset   = header.At<std::uint32_t>(Format::kSubCacheArrayOffset);
        const auto subCount    = header.At<std::uint32_t>(Format::kSubCacheArrayCount);
        const auto imageOffset = header.At<std::uint32_t>(Format::kImagesOffset);
        const auto imageCount  = header.At<std::uint32_t>(Format::kImagesCount);
        if (!subOffset || !subCount || !imageOffset || !imageCount)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Cache::Open", mainPath->string(),
                        "the header is truncated", "re-extract the cache");
        }

        for (std::uint32_t i = 0; i < *imageCount; ++i)
        {
            const auto info = header.At<Format::ImageInfo>(*imageOffset + i * sizeof(Format::ImageInfo));
            if (!info)
            {
                return std::unexpected(info.error());
            }
            const auto path = header.CString(info->PathFileOffset, 1024);
            if (!path)
            {
                return std::unexpected(path.error());
            }
            cache._images.push_back({std::string(*path), info->Address});
        }

        const std::string mainName = mainPath->filename().string();
        std::vector<Foundation::MappedFile> subcaches;
        for (std::uint32_t i = 0; i < *subCount; ++i)
        {
            const auto entry = header.At<Format::SubcacheEntry>(*subOffset + i * sizeof(Format::SubcacheEntry));
            if (!entry)
            {
                return std::unexpected(entry.error());
            }
            const std::string suffix(entry->FileSuffix, ::strnlen(entry->FileSuffix, sizeof(entry->FileSuffix)));
            auto sub = Foundation::MappedFile::Open(directory / (mainName + suffix));
            if (!sub)
            {
                return std::unexpected(sub.error());
            }
            const std::string expected = UuidText(std::as_bytes(std::span(entry->Uuid)));
            const std::string actual   = UuidText(sub->Bytes().subspan(Format::kUuid, 16));
            if (expected != actual)
            {
                return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "Cache::Open", sub->Path().string(),
                            std::format("subcache uuid {} differs from the main header's entry {}", actual, expected),
                            "the directory mixes files of two caches; re-extract it");
            }
            subcaches.push_back(std::move(*sub));
        }

        const std::string symbolUuid = UuidText(main->Bytes().subspan(Format::kSymbolFileUuid, 16));
        if (symbolUuid != std::string(32, '0'))
        {
            auto symbols = Foundation::MappedFile::Open(directory / (mainName + ".symbols"));
            if (!symbols)
            {
                return std::unexpected(symbols.error());
            }
            if (UuidText(symbols->Bytes().subspan(Format::kUuid, 16)) != symbolUuid)
            {
                return Fail(DiagnosticCode::Mismatch, Severity::NotVerified, "Cache::Open", symbols->Path().string(),
                            "the .symbols uuid differs from the main header's symbolFileUUID", "re-extract the cache");
            }
        }

        if (auto ok = cache.AddFile(std::move(*main)); !ok)
        {
            return std::unexpected(ok.error());
        }
        for (auto& sub : subcaches)
        {
            if (auto ok = cache.AddFile(std::move(sub)); !ok)
            {
                return std::unexpected(ok.error());
            }
        }
        cache._subcacheCount = subcaches.size();

        std::sort(cache._mappings.begin(), cache._mappings.end(),
                  [](const Mapping& a, const Mapping& b) { return a.Address < b.Address; });
        auto last = std::unique(cache._mappings.begin(), cache._mappings.end(), [](const Mapping& a, const Mapping& b) {
            return a.Address == b.Address && a.Size == b.Size;
        });
        cache._mappings.erase(last, cache._mappings.end());
        return cache;
    }

    const Cache::Mapping* Cache::Find(std::uint64_t va) const noexcept
    {
        auto it = std::upper_bound(_mappings.begin(), _mappings.end(), va,
                                   [](std::uint64_t value, const Mapping& m) { return value < m.Address; });
        if (it == _mappings.begin())
        {
            return nullptr;
        }
        --it;
        return va - it->Address < it->Size ? &*it : nullptr;
    }

    bool Cache::IsMapped(std::uint64_t va) const noexcept
    {
        return Find(va) != nullptr;
    }

    Expected<std::span<const std::byte>> Cache::Read(std::uint64_t va, std::size_t size) const
    {
        const Mapping* mapping = Find(va);
        if (mapping == nullptr)
        {
            return Fail(DiagnosticCode::Unmapped, Severity::NotVerified, "Cache::Read", Hex(va),
                        "no mapping of the cache holds the address", "query an address inside the cache");
        }
        if (mapping->Size - (va - mapping->Address) < size)
        {
            return Fail(DiagnosticCode::Unmapped, Severity::NotVerified, "Cache::Read", Hex(va),
                        std::format("{} bytes cross the end of the mapping", size), "read less");
        }
        return _files[mapping->File].Bytes().subspan(mapping->FileOffset + (va - mapping->Address), size);
    }

    Expected<std::uint64_t> Cache::U64(std::uint64_t va) const
    {
        const auto bytes = Read(va, 8);
        if (!bytes)
        {
            return std::unexpected(bytes.error());
        }
        return ByteReader(*bytes).At<std::uint64_t>(0);
    }

    Expected<const Cache::Mapping*> Cache::FindSlide(std::uint64_t slot, const char* operation) const
    {
        const Mapping* mapping = Find(slot);
        if (mapping == nullptr)
        {
            return Fail(DiagnosticCode::Unmapped, Severity::NotVerified, operation, Hex(slot),
                        "no mapping of the cache holds the slot", "query an address inside the cache");
        }
        if (!mapping->SlideInfo)
        {
            return Fail(DiagnosticCode::Unsupported, Severity::NotVerified, operation, Hex(slot),
                        "the mapping holding the slot carries no slide info", "ask about a data address");
        }
        if (mapping->SlideInfo->Version != 5)
        {
            return Fail(DiagnosticCode::Unsupported, Severity::NotVerified, operation, Hex(slot),
                        std::format("unhandled slide info version {}", mapping->SlideInfo->Version),
                        "add this slide info version to DyldSharedCache");
        }
        return mapping;
    }

    Expected<std::uint64_t> Cache::DecodePointer(std::uint64_t slot) const
    {
        const auto mapping = FindSlide(slot, "Cache::DecodePointer");
        if (!mapping)
        {
            return std::unexpected(mapping.error());
        }
        const auto raw = U64(slot);
        if (!raw)
        {
            return std::unexpected(raw.error());
        }
        if (*raw == 0)
        {
            return Fail(DiagnosticCode::NotFound, Severity::Failed, "Cache::DecodePointer", Hex(slot),
                        "the slot holds zero", "ask about a slot that holds a pointer");
        }
        const std::uint64_t target = (*mapping)->SlideInfo->ValueAdd + (*raw & Format::kSlideV5TargetMask);
        if (!IsMapped(target))
        {
            return Fail(DiagnosticCode::Unmapped, Severity::Failed, "Cache::DecodePointer", Hex(slot),
                        std::format("the decoded target {} is not mapped", Hex(target)), "the slot is data, not a pointer");
        }
        return target;
    }

    Expected<bool> Cache::IsRebased(std::uint64_t slot) const
    {
        const auto mapping = FindSlide(slot, "Cache::IsRebased");
        if (!mapping)
        {
            return std::unexpected(mapping.error());
        }
        const Slide&        slide = *(*mapping)->SlideInfo;
        const std::uint64_t base  = (*mapping)->Address;
        const std::uint64_t page  = (slot - base) / slide.PageSize;
        if (page >= slide.StartCount)
        {
            return false;
        }
        const ByteReader starts(_files[slide.File].Bytes());
        const auto start = starts.At<std::uint16_t>(slide.StartsAt + page * 2);
        if (!start)
        {
            return std::unexpected(start.error());
        }
        if (*start == Format::kSlideV5NoRebase)
        {
            return false;
        }
        std::uint64_t       link  = base + page * slide.PageSize + (*start / 8) * 8;
        const std::uint64_t limit = base + (page + 1) * slide.PageSize;
        while (link < limit)
        {
            if (link == slot)
            {
                return true;
            }
            const auto raw = U64(link);
            if (!raw)
            {
                return std::unexpected(raw.error());
            }
            const std::uint64_t delta = (*raw >> 52) & 0x7FF;
            if (delta == 0)
            {
                break;
            }
            link += delta * 8;
        }
        return false;
    }
}
