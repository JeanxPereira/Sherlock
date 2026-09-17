// Sherlock — tools/Sherlock/Source/Foundation/include/Foundation/ByteReader.h
// Bounds-checked little-endian reads from a byte span (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace Sherlock::Foundation
{
    class ByteReader
    {
    public:
        explicit ByteReader(std::span<const std::byte> bytes) noexcept : _bytes(bytes) {}

        template <class T>
            requires std::is_trivially_copyable_v<T>
        Expected<T> At(std::size_t offset) const
        {
            if (offset > _bytes.size() || _bytes.size() - offset < sizeof(T))
            {
                return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "ByteReader::At",
                            Hex(offset), "read past the end of the span", "check the length the caller passed");
            }
            T value{};
            std::memcpy(&value, _bytes.data() + offset, sizeof(T));
            return value;
        }

        // The zero-terminated string at `offset`, at most `limit` bytes long.
        Expected<std::string_view> CString(std::size_t offset, std::size_t limit) const
        {
            if (offset >= _bytes.size())
            {
                return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "ByteReader::CString",
                            Hex(offset), "string offset past the end of the span", "check the string table bounds");
            }
            const std::size_t available = std::min(limit, _bytes.size() - offset);
            const char*       start     = reinterpret_cast<const char*>(_bytes.data() + offset);
            const std::size_t length    = ::strnlen(start, available);
            return std::string_view(start, length);
        }

        std::size_t Size() const noexcept { return _bytes.size(); }

        std::span<const std::byte> Bytes() const noexcept { return _bytes; }

    private:
        std::span<const std::byte> _bytes;
    };
}
