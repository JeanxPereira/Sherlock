// Sherlock — tools/Sherlock/Source/Facts/include/Facts/ImageFacts.h
// One image's layer-1 facts: functions, symbols, calls, literal reads, coverage (derived).
#pragma once

#include <DyldSharedCache/Cache.h>
#include <Facts/Disassembler.h>
#include <MachO/Image.h>
#include <Store/Database.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Sherlock::Facts
{
    struct FunctionRow
    {
        std::uint64_t Address = 0;
        std::uint64_t Size    = 0;
    };

    struct CallRow
    {
        std::uint64_t                Site = 0, Caller = 0, Target = 0;
        std::optional<std::uint64_t> Island;
        std::string                  Via; // Direct | Island | Unresolved
    };

    struct LiteralRow
    {
        std::uint64_t                Site = 0, Target = 0;
        std::optional<std::uint64_t> Caller;
        std::string                  Kind;
        std::optional<double>        Value;
    };

    struct ImageFacts
    {
        std::string                      Path;
        std::vector<MachO::Segment>      Segments;
        std::vector<MachO::Section>      Sections;
        std::vector<FunctionRow>         Functions;
        std::vector<MachO::SymbolEntry>  Symbols;
        std::vector<CallRow>             Calls;
        std::vector<LiteralRow>          Literals;
        StreamCoverage                   Coverage;
    };

    Expected<ImageFacts> ExtractImage(const DyldSharedCache::Cache& cache, const DyldSharedCache::CacheImage& image,
                                      Disassembler& disassembler);

    Expected<void> WriteImageFacts(Store::Database& db, const ImageFacts& facts,
                                   const std::function<std::optional<std::string>(std::string_view)>& demangle);
}
