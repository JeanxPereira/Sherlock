// Sherlock — Source/DocumentIndex/include/DocumentIndex/ConceptFrontMatter.h
// The three-key front matter every docs/concepts/*.md page opens with, and the GENERATED marker (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Sherlock::DocumentIndex
{
    struct FrontMatter
    {
        std::string              Type;
        std::string              Title;
        std::vector<std::string> Aliases;
        std::size_t               BodyStart = 0; // byte offset right after the closing '---' line
    };

    // A file with no front matter (every docs/re/*.md laudo) returns an empty FrontMatter,
    // BodyStart 0 -- the caller distinguishes "concept page" from "laudo" by path, not by this.
    //
    // Three declared limits, none hit by the 210 docs/concepts pages parsed today (locked by
    // DocumentIndexGates.cpp, so a page that DOES hit one fails loud, not silently): a
    // docs/concepts page with no front matter of its own reads exactly like a laudo, empty and
    // successful; a duplicated `type`/`title`/`aliases` key takes the last occurrence, no
    // diagnostic; `aliases:` only reads the inline `[a, b, c]` form, never YAML's block style.
    Foundation::Expected<FrontMatter> ParseFrontMatter(std::string_view text);

    // The byte offset of "<!-- GENERATED" (concepts.py's own marker), or text.size() when absent.
    std::size_t GeneratedBlockStart(std::string_view text);
}
