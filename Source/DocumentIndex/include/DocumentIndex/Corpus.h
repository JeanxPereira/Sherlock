// Sherlock — Source/DocumentIndex/include/DocumentIndex/Corpus.h
// What a consumer asks DocumentIndex to read: its root, its collections and its seal tags (derived).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Sherlock::DocumentIndex
{
    // How a collection is read, never where it is: evidence splits markdown by heading, concept
    // reads one front-mattered page as one section, code extracts seals from "//" comments.
    enum class CollectionKind : std::uint8_t
    {
        Evidence,
        Concept,
        Code,
    };

    struct Collection
    {
        std::string              Path;       // relative to Corpus::Root, forward slashes, no trailing slash
        CollectionKind           Kind = CollectionKind::Evidence;
        std::vector<std::string> Extensions; // Code only: ".h", ".cpp", ...
    };

    // The consumer declares which tags exist and which one carries an image; the bracket shape
    // `[TAG] Image ... 0x...` and the cache-address shape are Sherlock's own.
    struct SealGrammar
    {
        std::vector<std::string> Tags;
        std::string              ImageTag; // empty: no tag carries an image
    };

    struct Corpus
    {
        std::filesystem::path      Root;
        std::vector<Collection>    Collections;  // indexed in this order
        std::optional<SealGrammar> Seals;        // absent: a code collection records its files, no seal
        std::string                ConfigSha256; // of the sherlock.json bytes; empty for an in-memory corpus
    };
}
