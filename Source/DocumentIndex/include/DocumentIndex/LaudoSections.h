// Sherlock — tools/Sherlock/Source/DocumentIndex/include/DocumentIndex/LaudoSections.h
// SplitDocument: one file -> its DocSection rows, laudo or concept page (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstddef>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Sherlock::DocumentIndex
{
    struct DocSection
    {
        std::string                 File; // repo-relative, forward slashes
        std::optional<std::string>  Number;
        std::string                 Title;
        std::size_t                 FirstLine = 0, LastLine = 0; // 1-based, inclusive
        std::string                 Text;                        // byte-exact slice of the file
    };

    // docs/re/<slug>.md: one DocSection per heading, spanning to the line before the next heading
    // whose Level is <= its own (decision 2 of the phase-2 plan) -- a deeper, unnumbered heading
    // stays inside its enclosing section's Text AND is independently addressable by its own title.
    // docs/concepts/<addr>.md (path contains "docs/concepts/"): exactly one DocSection for the
    // whole file, Number null, Title = the front matter's title, spanning line 1 through EOF.
    Foundation::Expected<std::vector<DocSection>> SplitDocument(const std::filesystem::path& file,
                                                                std::string_view              repoRelativePath);
    Foundation::Expected<std::vector<DocSection>> SplitDocument(std::istream&     stream,
                                                                std::string_view repoRelativePath,
                                                                std::string_view diagnosticSubject);
}
