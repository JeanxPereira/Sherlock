// Sherlock — tools/Sherlock/Source/DocumentIndex/include/DocumentIndex/Builder.h
// Walks docs/re, docs/concepts and Source/, writing Documents.db whole, in one transaction (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Sherlock::DocumentIndex
{
    struct DocumentsBuildReport
    {
        std::uint64_t SectionsWritten = 0, CitationsWritten = 0, SealsWritten = 0;
        std::uint64_t LaudoFilesRead = 0, LaudoFilesTotal = 0;
        std::uint64_t ConceptFilesRead = 0, ConceptFilesTotal = 0;
        std::uint64_t SourceFilesRead = 0, SourceFilesTotal = 0;
        std::string Head;
        double Seconds = 0;
    };

    Foundation::Expected<DocumentsBuildReport> BuildDocuments(const std::filesystem::path& repoRoot,
                                                              const std::filesystem::path& documentsPath);

    // The post-increment seam lets the gate prove an iterator advance failure never becomes end-of-range.
    Foundation::Expected<std::vector<std::filesystem::path>> WalkSourceForTesting(
        const std::filesystem::path& root, std::function<Foundation::Expected<void>()> afterIncrement);

    // The rule BuildDocuments uses to decide whether a markdown file under docs/re or
    // docs/concepts enters the corpus -- exported so a second walk (the CLI's staleness scan)
    // shares this exact rule instead of a second one that drifts from it.
    bool IsIndexedMarkdown(const std::filesystem::path& path);

    // The markdown counterpart of WalkSourceForTesting: lists the files BuildDocuments would
    // index under one directory (non-recursive, matching WalkMarkdown), for the same reason.
    Foundation::Expected<std::vector<std::filesystem::path>> WalkMarkdownForCli(const std::filesystem::path& dir);
}
