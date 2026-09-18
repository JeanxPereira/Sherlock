// Sherlock — tools/Sherlock/Source/DocumentIndex/include/DocumentIndex/SealExtractor.h
// A //-only port of References/scripts/lint_seals.py's blocks()/BIN/ADDR for Source/ (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Sherlock::DocumentIndex
{
    struct SealRow
    {
        std::string                  File; // repo-relative, forward slashes
        std::size_t                  Line = 0; // 1-based, the line carrying the tag itself
        std::string                  Tag;  // "BIN" | "KIT" | "OBS" | "API"
        std::optional<std::string>   Image;
        std::optional<std::string>   Symbol;  // always null in phase 2 -- see the plan's decision 5
        std::optional<std::uint64_t> Address; // the first cache-shaped address in the tag's own segment
    };

    // Reads `file` (already known to be Source/**/*.h|.cpp|.hpp -- the caller's job to filter) and
    // returns one SealRow per BIN/KIT/OBS/API-tagged occurrence found inside a "//" comment block,
    // mirroring lint_seals.py's blocks()/BIN/ADDR exactly for the BIN tag (Image and Address both
    // come from the same regex shapes, matched over the same per-tag segmentation: a tag's own
    // segment runs from its match end to the next occurrence of the SAME tag in the block, or to
    // the block's end -- lint_seals.py's own `bin_matches[idx + 1]` chaining, generalised here to
    // every tag kind it is applied to). Only the BIN tag ever carries an Image: lint_seals.py's own
    // BIN regex is the only one that captures one, so a KIT/OBS/API row leaves Image null (extending
    // Address extraction to those three tags, per the plan's decision 6, is a Sherlock-specific
    // widening; extending Image capture to them as well would invent a value lint_seals.py never
    // reports).
    Foundation::Expected<std::vector<SealRow>> ExtractSeals(const std::filesystem::path& file,
                                                             std::string_view             repoRelativePath);
    Foundation::Expected<std::vector<SealRow>> ExtractSeals(std::istream&     stream,
                                                            std::string_view repoRelativePath,
                                                            std::string_view diagnosticSubject);
}
