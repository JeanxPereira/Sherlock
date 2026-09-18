// Sherlock — tools/Sherlock/Source/DocumentIndex/include/DocumentIndex/Builder.h
// Walks docs/re, docs/concepts and Source/, writing Documents.db whole, in one transaction (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <string>

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
}
