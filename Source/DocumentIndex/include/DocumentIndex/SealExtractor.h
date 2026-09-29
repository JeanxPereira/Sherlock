// Sherlock — Source/DocumentIndex/include/DocumentIndex/SealExtractor.h
// Seals in "//" comment blocks: lint_seals.py's blocks()/BIN/ADDR shapes over the consumer's own tags (derived).
#pragma once

#include <DocumentIndex/Corpus.h>
#include <Foundation/Diagnostic.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace Sherlock::DocumentIndex
{
    struct SealRow
    {
        std::string                  File; // repo-relative, forward slashes
        std::size_t                  Line = 0; // 1-based, the line carrying the tag itself
        std::string                  Tag;  // one of the matcher's tags
        std::optional<std::string>   Image;
        std::optional<std::string>   Symbol;  // always null in phase 2 -- see the plan's decision 5
        std::optional<std::uint64_t> Address; // one cache-shaped address from the tag's own segment
    };

    // The consumer's tags compiled once: `[TAG]` for every declared tag, and the one tag whose seal
    // names an image.
    class SealMatcher
    {
    public:
        explicit SealMatcher(const SealGrammar& grammar);
        const std::regex&  Tags() const noexcept { return tags_; }
        const std::string& ImageTag() const noexcept { return imageTag_; }

    private:
        std::regex  tags_;
        std::string imageTag_;
    };

    // Reads `file` (already chosen by the code collection's extensions -- the caller's job) and
    // returns one SealRow per (file, line, tag, address) inside a "//" comment block, for the
    // matcher's tags; a bracketed word that is not a declared tag produces no row. Image and
    // Address come from lint_seals.py's own regex shapes over the same per-tag segmentation: a
    // segment with more than one cache-shaped address produces one row PER ADDRESS, all sharing
    // the segment's File/Line/Tag/Image; a segment with none produces one row, Address null. Only
    // the image tag ever carries an Image.
    Foundation::Expected<std::vector<SealRow>> ExtractSeals(const std::filesystem::path& file,
                                                             std::string_view             repoRelativePath,
                                                             const SealMatcher&           matcher);
    Foundation::Expected<std::vector<SealRow>> ExtractSeals(std::istream&      stream,
                                                            std::string_view  repoRelativePath,
                                                            std::string_view  diagnosticSubject,
                                                            const SealMatcher& matcher);
}
