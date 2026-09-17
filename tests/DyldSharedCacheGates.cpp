// Sherlock — tests/Sherlock/DyldSharedCacheGates.cpp
// The 26A5416b cache opens as one address space with the values measured for it.
#include <DyldSharedCache/Cache.h>

#include "SherlockHarness.h"

#include <array>
#include <fstream>

using namespace Sherlock;
using Foundation::DiagnosticCode;
using Foundation::Severity;

namespace
{
    void GateLayout(const DyldSharedCache::Cache& cache)
    {
        ExpectEq(cache.Uuid(), std::string_view("f06856bf36f8349c892d74941baa031b"), "main header uuid");
        ExpectEq(cache.SubcacheCount(), std::size_t{79}, "subcache entries");
        ExpectEq(cache.Images().size(), std::size_t{4083}, "image table entries");
        ExpectEq(cache.Base(), std::uint64_t{0x180000000}, "lowest mapped address");
        Expect(!cache.Images().empty() && cache.Images()[0].Path == "/usr/lib/libobjc.A.dylib" &&
                   cache.Images()[0].Header == 0x180404000,
               "first image is libobjc at 0x180404000");
    }

    void GateRead(const DyldSharedCache::Cache& cache)
    {
        const auto entry = cache.Read(0x240622d98, 4);
        const std::array<std::byte, 4> pacibsp{std::byte{0x7f}, std::byte{0x23}, std::byte{0x03}, std::byte{0xd5}};
        Expect(entry.has_value() && std::equal(entry->begin(), entry->end(), pacibsp.begin()),
               "0x240622d98 reads pacibsp (RegularBase entry)");
        const auto unmapped = cache.Read(0x7ffff0000, 1);
        Expect(!unmapped.has_value() && unmapped.error().Code == DiagnosticCode::Unmapped &&
                   unmapped.error().Level == Severity::NotVerified,
               "0x7ffff0000 is Unmapped and NOT VERIFIED");
        Expect(!cache.IsMapped(0x7ffff0000) && cache.IsMapped(0x240622d98), "IsMapped agrees with Read");
    }

    void GatePointers(const DyldSharedCache::Cache& cache)
    {
        ExpectEq(cache.U64(0x27945aee8).value_or(0), std::uint64_t{0x80140000a30edebc}, "raw GOT slot word");
        ExpectEq(cache.DecodePointer(0x27945aee8).value_or(0), std::uint64_t{0x2230edebc},
                 "GOT slot decodes to Material.Layer.opacity");
        ExpectEq(cache.DecodePointer(0x27945ae78).value_or(0), std::uint64_t{0x2230e8668},
                 "GOT slot decodes to Material.Layer.sdf");
        Expect(cache.IsRebased(0x27945aee8).value_or(false), "the GOT slot is on its rebase chain");
        const auto text = cache.IsRebased(0x240622d98);
        Expect(text.has_value() ? !*text : text.error().Code == DiagnosticCode::Unsupported,
               "a code address is not a rebased slot");
    }

    void GateRefusesForeignDirectory()
    {
        const auto dir = std::filesystem::temp_directory_path() / "SherlockNotACache";
        std::filesystem::create_directories(dir);
        {
            std::ofstream out(dir / "dyld_shared_cache_arm64e", std::ios::binary);
            out << "this is not a dyld shared cache header";
        }
        const auto opened = DyldSharedCache::Cache::Open(dir);
        Expect(!opened.has_value() && opened.error().Level == Severity::NotVerified,
               "a file without the dyld_v1 magic is NOT VERIFIED");
        std::filesystem::remove_all(dir);
    }
}

int main()
{
    auto cache = DyldSharedCache::Cache::Open(CacheDirectory());
    if (!cache)
    {
        std::printf("NOT VERIFIED: %s\n", cache.error().Format().c_str());
        return 2;
    }
    GateLayout(*cache);
    GateRead(*cache);
    GatePointers(*cache);
    GateRefusesForeignDirectory();
    return Finish();
}
