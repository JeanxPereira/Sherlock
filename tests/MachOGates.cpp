// Sherlock — tests/Sherlock/MachOGates.cpp
// Segments, sections, function starts and symbols of DesignLibrary and SwiftUICore; cache owner and image lookup.
#include <DyldSharedCache/Cache.h>
#include <MachO/Image.h>

#include "SherlockHarness.h"

using namespace Sherlock;
using Foundation::DiagnosticCode;
using Foundation::Severity;

namespace
{
    void GateDesignLibrary(const DyldSharedCache::Cache& cache)
    {
        const auto found = cache.FindImage("DesignLibrary");
        Expect(found.has_value() &&
                   (*found)->Path == "/System/Library/PrivateFrameworks/DesignLibrary.framework/Versions/A/DesignLibrary",
               "FindImage narrows DesignLibrary to its /System/Library path");
        if (!found)
        {
            return;
        }
        const auto image = MachO::Image::Parse(cache, (*found)->Header);
        Expect(image.has_value(), "DesignLibrary's header parses as Mach-O 64");
        if (!image)
        {
            return;
        }
        const auto* text = image->FindSection("__TEXT", "__text");
        Expect(text != nullptr, "DesignLibrary carries __TEXT.__text");
        if (text != nullptr)
        {
            ExpectEq(text->Address, std::uint64_t{0x240492428}, "__TEXT.__text address");
            ExpectEq(text->Size, std::uint64_t{0x1fc90c}, "__TEXT.__text size");
            ExpectEq(text->FullName(), std::string("__TEXT.__text"), "Section::FullName");
            ExpectEq(text->Size / 4, std::uint64_t{520771}, "520771 instruction words");
        }

        const auto starts = image->FunctionStarts(cache);
        Expect(starts.has_value(), "LC_FUNCTION_STARTS decodes");
        if (starts)
        {
            ExpectEq(starts->size(), std::size_t{11130}, "11130 function starts");
            ExpectEq(starts->front(), std::uint64_t{0x240492428}, "first start");
            ExpectEq(starts->back(), std::uint64_t{0x24068ed20}, "last start");
        }
    }

    void GateSwiftUICore(const DyldSharedCache::Cache& cache)
    {
        const auto found = cache.FindImage("SwiftUICore");
        Expect(found.has_value(), "FindImage locates SwiftUICore");
        if (!found)
        {
            return;
        }
        const auto image = MachO::Image::Parse(cache, (*found)->Header);
        Expect(image.has_value(), "SwiftUICore's header parses");
        if (!image)
        {
            return;
        }
        const auto symbols = image->Symbols(cache);
        Expect(symbols.has_value(), "LC_SYMTAB decodes");
        if (!symbols)
        {
            return;
        }
        ExpectEq(symbols->size(), std::size_t{216654}, "216654 filtered symbol table entries");
        bool sawOpacity = false;
        bool sawSdf     = false;
        for (const auto& entry : *symbols)
        {
            if (entry.Address == 0x2230edebc)
            {
                ExpectEq(entry.Name, std::string("_$s7SwiftUI8MaterialVAAE5LayerV7opacityyAESdF"), "opacity symbol name");
                sawOpacity = true;
            }
            if (entry.Address == 0x2230e8668)
            {
                ExpectEq(entry.Name, std::string("_$s7SwiftUI8MaterialVAAE5LayerV3sdfyA2E8SDFLayerVFZ"), "sdf symbol name");
                sawSdf = true;
            }
        }
        Expect(sawOpacity, "Material.Layer.opacity is present");
        Expect(sawSdf, "Material.Layer.sdf is present");
    }

    void GateOwner(const DyldSharedCache::Cache& cache)
    {
        const auto text = cache.Owner(0x240622d98);
        Expect(text.has_value() && text->Image->Path.ends_with("DesignLibrary") && text->Segment == "__TEXT",
               "0x240622d98 is owned by DesignLibrary __TEXT");
        const auto authConst = cache.Owner(0x27c198c20);
        Expect(authConst.has_value() && authConst->Image->Path.ends_with("DesignLibrary") &&
                   authConst->Segment == "__AUTH_CONST",
               "0x27c198c20 is owned by DesignLibrary __AUTH_CONST");
        Expect(!cache.Owner(0x27945aee8).has_value(), "the global GOT slot has no owning image");
        Expect(!cache.Owner(0x7ffff0000).has_value(), "an unmapped address has no owner");
    }

    void GateFindImageAmbiguity(const DyldSharedCache::Cache& cache)
    {
        const auto missing = cache.FindImage("NoSuchImageAnywhere");
        Expect(!missing.has_value() && missing.error().Code == DiagnosticCode::NotFound,
               "an image name that matches nothing is NotFound");
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
    try
    {
        GateDesignLibrary(*cache);
        GateSwiftUICore(*cache);
        GateOwner(*cache);
        GateFindImageAmbiguity(*cache);
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: an exception escaped a gate: %s\n", e.what());
        return 1;
    }
    return Finish();
}
