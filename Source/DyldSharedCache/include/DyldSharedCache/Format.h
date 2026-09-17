// Sherlock — tools/Sherlock/Source/DyldSharedCache/include/DyldSharedCache/Format.h
// Apple's on-disk structures of the dyld shared cache, from dyld's include/mach-o/dyld_cache_format.h.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Sherlock::DyldSharedCache::Format
{
    // dyld_cache_header field offsets (ctypes layout of Apple's struct, checked against 26A5416b).
    inline constexpr std::size_t kMagic                  = 0x000;
    inline constexpr std::size_t kMappingOffset          = 0x010;
    inline constexpr std::size_t kMappingCount           = 0x014;
    inline constexpr std::size_t kUuid                   = 0x058;
    inline constexpr std::size_t kCacheType              = 0x068;
    inline constexpr std::size_t kMappingWithSlideOffset = 0x138;
    inline constexpr std::size_t kMappingWithSlideCount  = 0x13c;
    inline constexpr std::size_t kSubCacheArrayOffset    = 0x188;
    inline constexpr std::size_t kSubCacheArrayCount     = 0x18c;
    inline constexpr std::size_t kSymbolFileUuid         = 0x190;
    inline constexpr std::size_t kImagesOffset           = 0x1c0;
    inline constexpr std::size_t kImagesCount            = 0x1c4;
    // A header whose mappingOffset passes this reaches cacheSubType, so its subcache entries carry a suffix.
    inline constexpr std::size_t kCacheSubTypeEnd        = 0x1c8;

    inline constexpr std::uint16_t kSlideV5NoRebase = 0xFFFF;
    inline constexpr std::uint64_t kSlideV5TargetMask = 0x3FFFFFFFFull;

    struct MappingInfo
    {
        std::uint64_t Address;
        std::uint64_t Size;
        std::uint64_t FileOffset;
        std::uint32_t MaxProt;
        std::uint32_t InitProt;
    };

    struct MappingAndSlideInfo
    {
        std::uint64_t Address;
        std::uint64_t Size;
        std::uint64_t FileOffset;
        std::uint64_t SlideInfoFileOffset;
        std::uint64_t SlideInfoFileSize;
        std::uint64_t Flags;
        std::uint32_t MaxProt;
        std::uint32_t InitProt;
    };

    struct ImageInfo
    {
        std::uint64_t Address;
        std::uint64_t ModTime;
        std::uint64_t Inode;
        std::uint32_t PathFileOffset;
        std::uint32_t Pad;
    };

    struct SubcacheEntry
    {
        std::uint8_t  Uuid[16];
        std::uint64_t CacheVmOffset;
        char          FileSuffix[32];
    };

    struct SlideInfo5
    {
        std::uint32_t Version;
        std::uint32_t PageSize;
        std::uint32_t PageStartsCount;
        std::uint64_t ValueAdd;
        // std::uint16_t PageStarts[PageStartsCount] follows at offset 24.
    };

    static_assert(sizeof(MappingInfo) == 32);
    static_assert(sizeof(MappingAndSlideInfo) == 56);
    static_assert(sizeof(ImageInfo) == 32);
    static_assert(sizeof(SubcacheEntry) == 56 && offsetof(SubcacheEntry, FileSuffix) == 24);
    static_assert(sizeof(SlideInfo5) == 24 && offsetof(SlideInfo5, ValueAdd) == 16);
}
