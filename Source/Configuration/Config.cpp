// Sherlock — Source/Configuration/Config.cpp
// Discover, Parse, Validate and Load for sherlock.json schema 1.
#include <Configuration/Config.h>

#include <DocumentIndex/Builder.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>

namespace Sherlock::Configuration
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        constexpr const char* kFileName = "sherlock.json";

        std::unexpected<Foundation::Diagnostic> Refuse(const std::filesystem::path& file, std::string reason,
                                                       std::string action)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Configuration::Parse", file.string(),
                        std::move(reason), std::move(action));
        }

        Foundation::Expected<void> OnlyKeys(const nlohmann::json& object, std::initializer_list<std::string_view> allowed,
                                            std::string_view where, const std::filesystem::path& file)
        {
            for (const auto& [key, value] : object.items())
            {
                if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                {
                    std::string list;
                    for (const auto name : allowed)
                    {
                        if (!list.empty()) list += ", ";
                        list += name;
                    }
                    return Refuse(file, std::format("{}unknown key \"{}\"", where, key),
                                  std::format("remove it; the keys here are {}", list));
                }
            }
            return {};
        }

        // Relative, forward slashes, no trailing slash, never above the root.
        Foundation::Expected<std::string> NormalizePath(const std::string& raw, std::string_view what, const std::filesystem::path& file)
        {
            const std::filesystem::path path(raw);
            if (raw.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
            {
                return Refuse(file, std::format("{} \"{}\": the path is not relative to sherlock.json's directory", what, raw),
                              "write the path relative to the directory holding sherlock.json");
            }
            auto normal = path.lexically_normal().generic_string();
            while (normal.size() > 1 && normal.back() == '/') normal.pop_back();
            if (normal == ".." || normal.starts_with("../"))
            {
                return Refuse(file, std::format("{} \"{}\": the path leaves sherlock.json's directory", what, raw),
                              "declare only paths under the consumer's root");
            }
            return normal;
        }

        bool IsTag(const std::string& tag)
        {
            if (tag.empty() || tag.front() < 'A' || tag.front() > 'Z') return false;
            return std::all_of(tag.begin(), tag.end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            });
        }
    }

    Foundation::Expected<std::filesystem::path> Discover(const std::filesystem::path& explicitFile,
                                                         const std::filesystem::path& start)
    {
        std::error_code error;
        if (!explicitFile.empty())
        {
            if (!std::filesystem::is_regular_file(explicitFile, error))
            {
                return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Configuration::Discover",
                            explicitFile.string(), "the --config file does not exist",
                            "pass the path of the consumer's sherlock.json");
            }
            return explicitFile;
        }
        std::filesystem::path dir = start;
        for (;;)
        {
            const auto candidate = dir / kFileName;
            if (std::filesystem::is_regular_file(candidate, error)) return candidate;
            const auto parent = dir.parent_path();
            if (parent.empty() || parent == dir) break;
            dir = parent;
        }
        return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Configuration::Discover", start.string(),
                    "no sherlock.json in this directory or any parent",
                    "run Sherlock from inside the consumer's tree, or pass --config <sherlock.json>");
    }

    Foundation::Expected<Config> Parse(std::string_view bytes, const std::filesystem::path& file)
    {
        nlohmann::json json;
        try
        {
            json = nlohmann::json::parse(bytes.begin(), bytes.end());
        }
        catch (const nlohmann::json::exception& error)
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Configuration::Parse", file.string(),
                        "sherlock.json is not valid JSON", "fix its syntax", error.what());
        }
        if (!json.is_object())
        {
            return Refuse(file, "sherlock.json is not a JSON object", "write one object with schema, sherlock and collections");
        }
        if (auto ok = OnlyKeys(json, {"schema", "sherlock", "towers", "collections", "seals"}, "", file); !ok)
        {
            return std::unexpected(ok.error());
        }
        for (const auto* key : {"schema", "sherlock", "collections"})
        {
            if (!json.contains(key))
            {
                return Refuse(file, std::format("missing key \"{}\"", key), "schema 1 requires schema, sherlock and collections");
            }
        }
        const auto& schema = json["schema"];
        if (!schema.is_number_integer() || schema.get<std::int64_t>() != 1)
        {
            return Refuse(file, std::format("\"schema\" is {}; this Sherlock reads schema 1", schema.dump()),
                          "write \"schema\": 1, or install the Sherlock that reads this schema");
        }
        if (!json["sherlock"].is_string())
        {
            return Refuse(file, "\"sherlock\" is not a string", "write the required Sherlock version, e.g. \"0.2\"");
        }

        Config config;
        config.File        = file;
        config.Root        = file.parent_path();
        config.Requirement = json["sherlock"].get<std::string>();
        config.Corpus.Root = config.Root;

        if (json.contains("towers"))
        {
            if (!json["towers"].is_string() || json["towers"].get<std::string>().empty())
            {
                return Refuse(file, "\"towers\" is not a path", "name the towers file relative to sherlock.json");
            }
            auto towers = NormalizePath(json["towers"].get<std::string>(), "\"towers\"", file);
            if (!towers) return std::unexpected(towers.error());
            config.Towers = config.Root / std::filesystem::path(*towers);
        }

        const auto& collections = json["collections"];
        if (!collections.is_array() || collections.empty())
        {
            return Refuse(file, "\"collections\" is not a non-empty array", "declare at least one collection");
        }
        for (std::size_t index = 0; index < collections.size(); ++index)
        {
            const auto& entry = collections[index];
            const auto  where = std::format("collection {}: ", index);
            if (!entry.is_object())
            {
                return Refuse(file, std::format("{}not an object", where), "write {\"path\": ..., \"kind\": ...}");
            }
            if (auto ok = OnlyKeys(entry, {"path", "kind", "extensions"}, where, file); !ok) return std::unexpected(ok.error());
            if (!entry.contains("path") || !entry["path"].is_string())
            {
                return Refuse(file, std::format("{}\"path\" is missing or not a string", where), "name the collection's directory");
            }
            if (!entry.contains("kind") || !entry["kind"].is_string())
            {
                return Refuse(file, std::format("{}\"kind\" is missing or not a string", where), "a kind is evidence, concept or code");
            }
            auto path = NormalizePath(entry["path"].get<std::string>(), "collection", file);
            if (!path) return std::unexpected(path.error());

            DocumentIndex::Collection collection;
            collection.Path = *path;
            const auto kind = entry["kind"].get<std::string>();
            if (kind == "evidence") collection.Kind = DocumentIndex::CollectionKind::Evidence;
            else if (kind == "concept") collection.Kind = DocumentIndex::CollectionKind::Concept;
            else if (kind == "code") collection.Kind = DocumentIndex::CollectionKind::Code;
            else
            {
                return Refuse(file, std::format("collection \"{}\": invalid \"kind\" \"{}\"", collection.Path, kind),
                              "a kind is evidence, concept or code");
            }
            if (entry.contains("extensions"))
            {
                if (collection.Kind != DocumentIndex::CollectionKind::Code)
                {
                    return Refuse(file, std::format("collection \"{}\": \"extensions\" applies to kind code only", collection.Path),
                                  "remove it; evidence and concept collections read .md files");
                }
                if (!entry["extensions"].is_array())
                {
                    return Refuse(file, std::format("collection \"{}\": \"extensions\" is not an array", collection.Path),
                                  "list the extensions, e.g. [\".h\", \".cpp\"]");
                }
                for (const auto& extension : entry["extensions"])
                {
                    if (!extension.is_string() || extension.get<std::string>().size() < 2 ||
                        !extension.get<std::string>().starts_with("."))
                    {
                        return Refuse(file, std::format("collection \"{}\": extension {} does not start with a dot",
                                                        collection.Path, extension.dump()),
                                      "write each extension with its dot, e.g. \".h\"");
                    }
                    collection.Extensions.push_back(extension.get<std::string>());
                }
            }
            if (collection.Kind == DocumentIndex::CollectionKind::Code && collection.Extensions.empty())
            {
                return Refuse(file, std::format("collection \"{}\": kind code needs \"extensions\"", collection.Path),
                              "list the extensions whose \"//\" comments carry seals");
            }
            config.Corpus.Collections.push_back(std::move(collection));
        }

        if (json.contains("seals"))
        {
            const auto& seals = json["seals"];
            if (!seals.is_object())
            {
                return Refuse(file, "\"seals\" is not an object", "write {\"tags\": [...], \"imageTag\": ...}");
            }
            if (auto ok = OnlyKeys(seals, {"tags", "imageTag"}, "seals: ", file); !ok) return std::unexpected(ok.error());
            if (!seals.contains("tags") || !seals["tags"].is_array())
            {
                return Refuse(file, "seals: \"tags\" is missing or not an array", "list the seal tags, e.g. [\"BIN\"]");
            }
            if (seals["tags"].empty())
            {
                return Refuse(file, "seals: \"tags\" is empty", "list at least one tag, or remove \"seals\"");
            }
            DocumentIndex::SealGrammar grammar;
            for (const auto& tag : seals["tags"])
            {
                if (!tag.is_string() || !IsTag(tag.get<std::string>()))
                {
                    return Refuse(file, std::format("seals: tag {} is not an upper-case word", tag.dump()),
                                  "a tag is [A-Z][A-Z0-9_]*, written without its brackets");
                }
                grammar.Tags.push_back(tag.get<std::string>());
            }
            if (seals.contains("imageTag"))
            {
                if (!seals["imageTag"].is_string())
                {
                    return Refuse(file, "seals: \"imageTag\" is not a string", "name one of the declared tags");
                }
                grammar.ImageTag = seals["imageTag"].get<std::string>();
                if (std::find(grammar.Tags.begin(), grammar.Tags.end(), grammar.ImageTag) == grammar.Tags.end())
                {
                    return Refuse(file, std::format("seals: \"imageTag\" \"{}\" is not one of \"tags\"", grammar.ImageTag),
                                  "name one of the declared tags");
                }
            }
            config.Corpus.Seals = std::move(grammar);
        }
        return config;
    }

    Foundation::Expected<void> Validate(const Config& config)
    {
        std::error_code error;
        if (!config.Towers.empty() && !std::filesystem::is_regular_file(config.Towers, error))
        {
            return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Configuration::Validate", config.File.string(),
                        std::format("\"towers\" names {} and no such file exists", config.Towers.string()),
                        "fix the towers path in sherlock.json");
        }
        for (const auto& collection : config.Corpus.Collections)
        {
            auto relative = std::filesystem::path(collection.Path);
            const auto directory = config.Root / relative.make_preferred();
            if (!std::filesystem::is_directory(directory, error))
            {
                return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Configuration::Validate", config.File.string(),
                            std::format("collection \"{}\" names {} and no such directory exists", collection.Path,
                                        directory.string()),
                            "fix the collection's path in sherlock.json");
            }
            auto files = DocumentIndex::WalkCollection(config.Root, collection);
            if (!files) return std::unexpected(files.error());
            if (files->empty())
            {
                return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "Configuration::Validate", config.File.string(),
                            std::format("collection \"{}\" holds no file Sherlock reads -- an empty collection is almost "
                                        "always a wrong path", collection.Path),
                            "fix the path, or remove the collection from sherlock.json");
            }
        }
        return {};
    }

    Foundation::Expected<Config> Load(const std::filesystem::path& file, std::string_view runningVersion)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(file, error);
        if (error)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "Configuration::Load", file.string(),
                        "the path of sherlock.json cannot be resolved", "check the path");
        }
        std::ifstream stream(absolute, std::ios::binary);
        if (!stream)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "Configuration::Load", absolute.string(),
                        "sherlock.json cannot be opened", "check the file is readable");
        }
        const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (stream.bad())
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "Configuration::Load", absolute.string(),
                        "sherlock.json cannot be read completely", "check the file is readable");
        }
        auto sha = Sha256Hex(bytes);
        if (!sha) return std::unexpected(sha.error());
        auto config = Parse(bytes, absolute);
        if (!config) return std::unexpected(config.error());
        config->Corpus.ConfigSha256 = *sha;
        if (auto ok = CheckRequirement(config->Requirement, runningVersion); !ok) return std::unexpected(ok.error());
        if (auto ok = Validate(*config); !ok) return std::unexpected(ok.error());
        return config;
    }

    std::filesystem::path DefaultDocuments(const Config& config)
    {
        return config.Root / "build" / "Sherlock" / "Documents.db";
    }

    Foundation::Diagnostic NotLoaded()
    {
        return {DiagnosticCode::NotFound, Severity::NotVerified, "Configuration", "",
                "no sherlock.json was loaded for this command", "pass --config <sherlock.json>"};
    }
}
