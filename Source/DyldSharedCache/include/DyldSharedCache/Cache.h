// Sherlock — tools/Sherlock/Source/DyldSharedCache/include/DyldSharedCache/Cache.h
// The main cache file and its subcaches, read as one virtual address space (Apple name: dyld shared cache).
#pragma once

#include <Foundation/AddressSpace.h>
#include <Foundation/MappedFile.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Sherlock::DyldSharedCache
{
    using Foundation::Expected;

    struct CacheImage
    {
        std::string   Path;
        std::uint64_t Header = 0;
    };

    class Cache final : public Foundation::AddressSpace
    {
    public:
        static Expected<Cache> Open(const std::filesystem::path& directory);

        Cache(Cache&&) noexcept            = default;
        Cache& operator=(Cache&&) noexcept = default;
        Cache(const Cache&)                = delete;
        Cache& operator=(const Cache&)     = delete;

        Expected<std::span<const std::byte>> Read(std::uint64_t va, std::size_t size) const override;
        Expected<std::uint64_t>              U64(std::uint64_t va) const;
        bool                                 IsMapped(std::uint64_t va) const noexcept;
        Expected<std::uint64_t>              DecodePointer(std::uint64_t slot) const;
        Expected<bool>                       IsRebased(std::uint64_t slot) const;

        const std::vector<CacheImage>& Images() const noexcept { return _images; }
        std::string_view               Uuid() const noexcept { return _uuid; }
        std::size_t                    SubcacheCount() const noexcept { return _subcacheCount; }
        std::size_t                    MappingCount() const noexcept { return _mappings.size(); }
        std::uint64_t                  Base() const noexcept { return _mappings.empty() ? 0 : _mappings.front().Address; }

    private:
        struct Slide
        {
            std::uint32_t Version    = 0;
            std::uint32_t PageSize   = 0;
            std::uint64_t ValueAdd   = 0;
            std::size_t   File       = 0;
            std::size_t   StartsAt   = 0; // file offset of page_starts[0]
            std::uint32_t StartCount = 0;
        };

        struct Mapping
        {
            std::uint64_t        Address    = 0;
            std::uint64_t        Size       = 0;
            std::size_t          File       = 0;
            std::uint64_t        FileOffset = 0;
            std::optional<Slide> SlideInfo;
        };

        Cache() = default;
        const Mapping*                       Find(std::uint64_t va) const noexcept;
        Expected<const Mapping*>             FindSlide(std::uint64_t slot, const char* operation) const;
        Expected<void>                       AddFile(Foundation::MappedFile file);

        std::vector<Foundation::MappedFile> _files;
        std::vector<Mapping>                _mappings;
        std::vector<CacheImage>             _images;
        std::string                         _uuid;
        std::size_t                         _subcacheCount = 0;
    };
}
