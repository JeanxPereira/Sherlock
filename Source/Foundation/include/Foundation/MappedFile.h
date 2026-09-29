// Sherlock — Source/Foundation/include/Foundation/MappedFile.h
// A whole file mapped read-only; the handles close with the object (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstddef>
#include <filesystem>
#include <span>

namespace Sherlock::Foundation
{
    class MappedFile
    {
    public:
        static Expected<MappedFile> Open(const std::filesystem::path& path);

        MappedFile(MappedFile&& other) noexcept;
        MappedFile& operator=(MappedFile&& other) noexcept;
        MappedFile(const MappedFile&)            = delete;
        MappedFile& operator=(const MappedFile&) = delete;
        ~MappedFile();

        std::span<const std::byte> Bytes() const noexcept { return {_view, _size}; }

        const std::filesystem::path& Path() const noexcept { return _path; }

    private:
        MappedFile() = default;
        void Release() noexcept;

        void*                 _file    = nullptr;
        void*                 _mapping = nullptr;
        const std::byte*      _view    = nullptr;
        std::size_t           _size    = 0;
        std::filesystem::path _path;
    };
}
