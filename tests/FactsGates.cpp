// Sherlock — tests/Sherlock/FactsGates.cpp
// Island resolution, out-of-text calls, literal reads and coverage against the measured controls.
#include <DyldSharedCache/Cache.h>
#include <Facts/BranchIsland.h>
#include <Facts/Disassembler.h>
#include <Facts/ImageFacts.h>
#include <Facts/LiteralReaders.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include "SherlockHarness.h"

using namespace Sherlock;
using Foundation::DiagnosticCode;
using Foundation::Severity;

namespace
{
    void GateSignedLiteralOffset()
    {
        Facts::LiteralTracker tracker;
        std::vector<Facts::LiteralRead> reads;
        tracker.Feed({0x100, "adrp", "x8, #0x1000"}, reads);
        tracker.Feed({0x104, "ldur", "d0, [x8, #-8]"}, reads);
        ExpectEq(reads.size(), std::size_t{1}, "a negative ldur displacement produces one literal read");
        if (!reads.empty())
        {
            ExpectEq(reads.front().Target, std::uint64_t{0xff8}, "the signed displacement is applied without wrap");
        }
    }

    void GateIslandControls(const DyldSharedCache::Cache& cache, Facts::Disassembler& disassembler)
    {
        const auto a = Facts::ResolveIsland(cache, disassembler, 0x24808c3e0);
        Expect(a.has_value() && a->has_value() && **a == 0x2230edebc, "island 0x24808c3e0 resolves to Material.Layer.opacity");
        const auto b = Facts::ResolveIsland(cache, disassembler, 0x24808c310);
        Expect(b.has_value() && b->has_value() && **b == 0x2230e8668, "island 0x24808c310 resolves to Material.Layer.sdf");
    }

    void GateResolveLayersFunction(const DyldSharedCache::Cache& cache, Facts::Disassembler& disassembler)
    {
        const auto found = cache.FindImage("DesignLibrary");
        Expect(found.has_value(), "DesignLibrary resolves for the resolveLayers control");
        if (!found)
        {
            return;
        }
        const auto image = Facts::ExtractImage(cache, *(*found), disassembler);
        Expect(image.has_value(), "ExtractImage runs over DesignLibrary");
        if (!image)
        {
            return;
        }
        // litref.py DesignLibrary --pools measures 520734/520771 words, not 100%: 37 words in
        // __TEXT.__text are not instructions, and the plan's "decodes 100%" premise does not hold here.
        ExpectEq(image->Coverage.Decoded, std::uint64_t{520734}, "DesignLibrary decoded words");
        ExpectEq(image->Coverage.Total, std::uint64_t{520771}, "DesignLibrary total __text words");

        std::size_t outOfText = 0;
        bool        sawOpacityIsland = false;
        bool        sawSdfIsland     = false;
        for (const auto& call : image->Calls)
        {
            if (call.Caller != 0x2406780f0)
            {
                continue;
            }
            if (call.Via == "Direct")
            {
                continue;
            }
            ++outOfText;
            if (call.Site == 0x240679e58 && call.Target == 0x2230edebc)
            {
                sawOpacityIsland = true;
            }
            if (call.Site == 0x240679df8 && call.Target == 0x2230e8668)
            {
                sawSdfIsland = true;
            }
        }
        ExpectEq(outOfText, std::size_t{83}, "resolveLayers makes 83 out-of-text calls");
        Expect(sawOpacityIsland, "0x240679e58 resolves to Material.Layer.opacity");
        Expect(sawSdfIsland, "0x240679df8 resolves to Material.Layer.sdf");
    }

    void GateSystemBannerLiterals(const DyldSharedCache::Cache& cache, Facts::Disassembler& disassembler)
    {
        const auto found = cache.FindImage("SystemBannerUI");
        Expect(found.has_value(), "SystemBannerUI resolves");
        if (!found)
        {
            return;
        }
        const auto image = Facts::ExtractImage(cache, *(*found), disassembler);
        Expect(image.has_value(), "ExtractImage runs over SystemBannerUI");
        if (!image)
        {
            return;
        }
        ExpectEq(image->Coverage.Decoded, std::uint64_t{77633}, "SystemBannerUI decoded words");
        ExpectEq(image->Coverage.Total, std::uint64_t{77633}, "SystemBannerUI total words");

        std::size_t hits = 0;
        for (const auto& literal : image->Literals)
        {
            if (literal.Target >= 0x29f60f388 && literal.Target < 0x29f60f480)
            {
                ++hits;
            }
        }
        ExpectEq(hits, std::size_t{24}, "24 literal reads land in the shadow pool");
    }

    void GateWriteRoundTrip(const DyldSharedCache::Cache& cache, Facts::Disassembler& disassembler)
    {
        const auto found = cache.FindImage("SystemBannerUI");
        if (!found)
        {
            return;
        }
        const auto image = Facts::ExtractImage(cache, *(*found), disassembler);
        if (!image)
        {
            return;
        }
        const auto path = std::filesystem::temp_directory_path() / "SherlockFactsGate.db";
        for (const char* suffix : {"", "-wal", "-shm"})
        {
            std::filesystem::remove(path.string() + suffix);
        }
        auto db = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "the round-trip database opens");
        Expect(Store::CreateImageStore(*db, image->Path, "test-cache").has_value(), "the image schema is created");
        Expect(Facts::WriteImageFacts(*db, *image, [](std::string_view) { return std::nullopt; }).has_value(),
               "WriteImageFacts runs");
        ExpectEq(db->ScalarInt("SELECT count(*) FROM Function").value(),
                 static_cast<std::int64_t>(image->Functions.size()), "Function row count matches");
        ExpectEq(db->ScalarInt("SELECT count(*) FROM Call").value(), static_cast<std::int64_t>(image->Calls.size()),
                 "Call row count matches");
        ExpectEq(db->ScalarInt("SELECT count(*) FROM LiteralRef").value(),
                 static_cast<std::int64_t>(image->Literals.size()), "LiteralRef row count matches");
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
    auto disassembler = Facts::Disassembler::Create();
    if (!disassembler)
    {
        std::printf("NOT VERIFIED: %s\n", disassembler.error().Format().c_str());
        return 2;
    }
    try
    {
        GateSignedLiteralOffset();
        GateIslandControls(*cache, *disassembler);
        GateResolveLayersFunction(*cache, *disassembler);
        GateSystemBannerLiterals(*cache, *disassembler);
        GateWriteRoundTrip(*cache, *disassembler);
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: an exception escaped a gate: %s\n", e.what());
        return 1;
    }
    return Finish();
}
