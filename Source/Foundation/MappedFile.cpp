// Sherlock — Source/Foundation/MappedFile.cpp
// Win32 file mapping behind MappedFile.
#include <Foundation/MappedFile.h>

#include <windows.h>

#include <utility>

namespace Sherlock::Foundation
{
    Expected<MappedFile> MappedFile::Open(const std::filesystem::path& path)
    {
        // The \\?\ prefix lifts the 260-character limit on nested corpus paths.
        const std::wstring longPath = L"\\\\?\\" + std::filesystem::absolute(path).wstring();

        MappedFile mapped;
        mapped._path = path;
        mapped._file = ::CreateFileW(longPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mapped._file == INVALID_HANDLE_VALUE)
        {
            mapped._file = nullptr;
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "MappedFile::Open", path.string(),
                        "the file cannot be opened", "check the path and that no process holds it exclusively",
                        LastSystemError());
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(mapped._file, &size))
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "MappedFile::Open", path.string(),
                        "the file size cannot be read", "check the file", LastSystemError());
        }
        mapped._size = static_cast<std::size_t>(size.QuadPart);
        if (mapped._size == 0)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "MappedFile::Open", path.string(),
                        "the file is empty", "replace the file");
        }

        mapped._mapping = ::CreateFileMappingW(mapped._file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapped._mapping == nullptr)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "MappedFile::Open", path.string(),
                        "the mapping cannot be created", "check the free address space", LastSystemError());
        }

        mapped._view = static_cast<const std::byte*>(::MapViewOfFile(mapped._mapping, FILE_MAP_READ, 0, 0, 0));
        if (mapped._view == nullptr)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "MappedFile::Open", path.string(),
                        "the view cannot be mapped", "check the free address space", LastSystemError());
        }
        return mapped;
    }

    MappedFile::MappedFile(MappedFile&& other) noexcept
        : _file(std::exchange(other._file, nullptr)),
          _mapping(std::exchange(other._mapping, nullptr)),
          _view(std::exchange(other._view, nullptr)),
          _size(std::exchange(other._size, 0)),
          _path(std::move(other._path))
    {
    }

    MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            _file    = std::exchange(other._file, nullptr);
            _mapping = std::exchange(other._mapping, nullptr);
            _view    = std::exchange(other._view, nullptr);
            _size    = std::exchange(other._size, 0);
            _path    = std::move(other._path);
        }
        return *this;
    }

    MappedFile::~MappedFile()
    {
        Release();
    }

    void MappedFile::Release() noexcept
    {
        if (_view != nullptr)
        {
            ::UnmapViewOfFile(_view);
            _view = nullptr;
        }
        if (_mapping != nullptr)
        {
            ::CloseHandle(_mapping);
            _mapping = nullptr;
        }
        if (_file != nullptr)
        {
            ::CloseHandle(_file);
            _file = nullptr;
        }
        _size = 0;
    }
}
