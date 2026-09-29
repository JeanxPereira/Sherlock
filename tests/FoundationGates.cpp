// Sherlock — tests/FoundationGates.cpp
// Diagnostic text, bounds-checked reads, and mapping lifetime.
#include <Foundation/ByteReader.h>
#include <Foundation/Diagnostic.h>
#include <Foundation/MappedFile.h>

#include "SherlockHarness.h"

#include <windows.h>

#include <array>
#include <fstream>

using namespace Sherlock::Foundation;

namespace
{
    void GateDiagnosticFormat()
    {
        auto failure = Fail(DiagnosticCode::Unmapped, Severity::NotVerified, "Cache::Read", "0x7ffff0000",
                            "no mapping holds the address", "query an address inside the cache", "5: denied");
        ExpectEq(failure.error().Format(),
                 std::string("Unmapped in Cache::Read (0x7ffff0000): no mapping holds the address -- "
                             "query an address inside the cache [system: 5: denied]"),
                 "Diagnostic::Format");
    }

    void GateByteReader()
    {
        const std::array<std::byte, 6> bytes{std::byte{0x78}, std::byte{0x56}, std::byte{0x34},
                                             std::byte{0x12}, std::byte{0x41}, std::byte{0x00}};
        const ByteReader reader(bytes);
        ExpectEq(reader.At<std::uint32_t>(0).value(), 0x12345678u, "At<u32> is little-endian");
        Expect(!reader.At<std::uint32_t>(3).has_value(), "At<u32> refuses a read that crosses the end");
        ExpectEq(reader.CString(4, 16).value(), std::string_view("A"), "CString stops at the terminator");
        Expect(!reader.CString(6, 16).has_value(), "CString refuses an offset at the end");
    }

    void GateMappedFileLifetime()
    {
        const auto path = std::filesystem::temp_directory_path() / "SherlockMappedFileGate.bin";
        {
            std::ofstream out(path, std::ios::binary);
            out << "sherlock";
        }

        const auto missing = MappedFile::Open(path.string() + ".absent");
        Expect(!missing.has_value() && missing.error().Code == DiagnosticCode::Io, "a missing file is an Io diagnostic");

        DWORD before = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &before);
        for (int i = 0; i < 200; ++i)
        {
            auto mapped = MappedFile::Open(path);
            Expect(mapped.has_value() && mapped->Bytes().size() == 8, "the file maps with its size");
            MappedFile moved = std::move(*mapped);
            Expect(moved.Bytes()[0] == std::byte{'s'}, "a moved mapping keeps its view");
        }
        DWORD after = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &after);
        ExpectEq(after, before, "200 open/close cycles leak no handle");
        std::filesystem::remove(path);
    }
}

int main()
{
    try
    {
        GateDiagnosticFormat();
        GateByteReader();
        GateMappedFileLifetime();
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: an exception escaped a gate: %s\n", e.what());
        return 1;
    }
    return Finish();
}
