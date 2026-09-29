// Sherlock — Source/Configuration/include/Configuration/Config.h
// sherlock.json: discovery, strict validation, the version requirement and the bytes' SHA-256 (derived).
#pragma once

#include <DocumentIndex/Corpus.h>
#include <Foundation/Diagnostic.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace Sherlock::Configuration
{
    struct Config
    {
        std::filesystem::path File;        // the sherlock.json read
        std::filesystem::path Root;        // its directory; every declared path is relative to it
        std::string           Requirement; // the "sherlock" key, MAJOR.MINOR[.PATCH]
        std::filesystem::path Towers;      // resolved against Root; empty when the file declares none
        DocumentIndex::Corpus Corpus;
    };

    // --config when given, else the first sherlock.json walking up from `start`.
    Foundation::Expected<std::filesystem::path> Discover(const std::filesystem::path& explicitFile,
                                                         const std::filesystem::path& start);

    // The shape: keys, types, kinds, schema, relative paths. Reads nothing from disk.
    Foundation::Expected<Config> Parse(std::string_view bytes, const std::filesystem::path& file);

    // What the shape names exists: the towers file, each collection's directory, and at least one
    // file Sherlock reads in each collection -- an empty collection is almost always a wrong path.
    Foundation::Expected<void> Validate(const Config& config);

    // A 0.x requirement names a series: "0.2" accepts 0.2.0 and every later 0.2.x, never 0.1 or 0.3.
    // From 1.0 the major must match and the minor.patch must reach the requirement.
    Foundation::Expected<void> CheckRequirement(std::string_view required, std::string_view running);

    Foundation::Expected<std::string> Sha256Hex(std::string_view bytes);

    // Read the bytes, hash them, Parse, CheckRequirement, Validate -- in that order.
    Foundation::Expected<Config> Load(const std::filesystem::path& file, std::string_view runningVersion);

    std::filesystem::path DefaultDocuments(const Config& config);

    // What a command that loaded no configuration answers when layer 3 asks for one.
    Foundation::Diagnostic NotLoaded();
}
