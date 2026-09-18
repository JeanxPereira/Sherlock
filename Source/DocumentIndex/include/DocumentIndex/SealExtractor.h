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
        std::optional<std::uint64_t> Address; // one cache-shaped address from the tag's own segment
    };

    // Reads `file` (already known to be Source/**/*.h|.cpp|.hpp -- the caller's job to filter) and
    // returns one SealRow per (file, line, tag, address) found inside a "//" comment block, for the
    // BIN/KIT/OBS/API/INF/DEMO/ASSUMPTION tags (lint_seals.py's TAG also refuses RE/DOC/WEB as
    // unknown-tag; those never produce a row). Image and Address come from lint_seals.py's own
    // regex shapes, over the same per-tag segmentation (SealExtractor.cpp has the detail): a
    // segment with more than one cache-shaped address produces one row PER ADDRESS, all sharing
    // the segment's File/Line/Tag/Image; a segment with none produces exactly one row, Address
    // null. Only BIN ever carries an Image -- lint_seals.py's own BIN regex is the only one that
    // captures one (extending Address extraction to the other tags is a Sherlock-specific
    // widening, decision 6; extending Image the same way would invent a value it never reports).
    Foundation::Expected<std::vector<SealRow>> ExtractSeals(const std::filesystem::path& file,
                                                             std::string_view             repoRelativePath);
    Foundation::Expected<std::vector<SealRow>> ExtractSeals(std::istream&     stream,
                                                            std::string_view repoRelativePath,
                                                            std::string_view diagnosticSubject);
}
