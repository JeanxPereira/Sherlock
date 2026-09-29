// Sherlock — Source/DocumentIndex/include/DocumentIndex/Builder.h
// Walks a consumer's collections, writing Documents.db whole, in one transaction (derived).
#pragma once

#include <DocumentIndex/Corpus.h>
#include <Foundation/Diagnostic.h>
#include <Store/Database.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Sherlock::DocumentIndex
{
    struct CollectionReport
    {
        std::string   Path;
        std::uint64_t Read = 0, Total = 0;
    };

    struct DocumentsBuildReport
    {
        std::uint64_t                 SectionsWritten = 0, CitationsWritten = 0, SealsWritten = 0;
        std::vector<CollectionReport> Collections; // one per declared collection, in declaration order
        std::string                   Head;
        double                        Seconds = 0;
    };

    Foundation::Expected<DocumentsBuildReport> BuildDocuments(const Corpus&                corpus,
                                                              const std::filesystem::path& documentsPath);

    // The files BuildDocuments reads for one collection -- the single inclusion rule the build,
    // status's staleness scan and the configuration's empty-collection refusal share.
    Foundation::Expected<std::vector<std::filesystem::path>> WalkCollection(const std::filesystem::path& root,
                                                                            const Collection&            collection);

    // A code collection's walk: recursive, build/lab/.git skipped, `extensions` matched exactly.
    // The post-increment seam lets the gate prove an iterator advance failure never becomes end-of-range.
    Foundation::Expected<std::vector<std::filesystem::path>> WalkCode(
        const std::filesystem::path& dir, const std::vector<std::string>& extensions,
        std::function<Foundation::Expected<void>()> afterIncrement = {});

    // The rule an evidence or concept collection uses to decide whether a markdown file enters.
    bool IsIndexedMarkdown(const std::filesystem::path& path);

    // The files BuildDocuments would index under one markdown directory (non-recursive).
    Foundation::Expected<std::vector<std::filesystem::path>> WalkMarkdownForCli(const std::filesystem::path& dir);

    // The ConfigSha256 a Documents.db was built with; empty for a store that records none.
    std::string BuiltConfigSha256(Store::Database& documents);
}
