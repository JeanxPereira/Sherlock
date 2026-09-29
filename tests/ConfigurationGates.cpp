// Sherlock — tests/ConfigurationGates.cpp
// sherlock.json schema 1: the shape Parse accepts, every refusal it names, the version rule, the
// SHA-256 of the bytes, discovery walking up, and Validate and Load over a real tree.
#include <Configuration/Config.h>

#include "SherlockHarness.h"

#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace Sherlock;

namespace
{
    const std::filesystem::path kFile("C:/consumer/sherlock.json");
    const std::string kCollections =
        R"([{"path": "docs/re", "kind": "evidence"}, {"path": "docs/concepts", "kind": "concept"}, {"path": "Source", "kind": "code", "extensions": [".h", ".cpp"]}])";
    const std::string kValid = R"({"schema": 1, "sherlock": "0.2", "towers": "towers.json", "collections": )" +
                               kCollections + R"(, "seals": {"tags": ["BIN", "KIT"], "imageTag": "BIN"}})";

    // A replacement that finds nothing would test the valid file and read as a pass.
    std::string Replaced(std::string text, std::string_view from, std::string_view to)
    {
        const auto at = text.find(from);
        if (at == std::string::npos)
        {
            std::printf("FAIL: the gate's own replacement '%.*s' matched nothing\n", static_cast<int>(from.size()), from.data());
            std::exit(1);
        }
        return text.replace(at, from.size(), to);
    }

    void ExpectRefused(const std::string& json, std::string_view fragment, std::string_view what)
    {
        const auto config = Configuration::Parse(json, kFile);
        const std::string got = config ? std::string("accepted") : config.error().Format();
        Expect(!config && config.error().Level == Foundation::Severity::NotVerified && got.find(fragment) != std::string::npos,
               std::string(what) + " -- expected NOT VERIFIED naming '" + std::string(fragment) + "', got: " + got);
    }

    void Write(const std::filesystem::path& file, std::string_view text)
    {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

    void GateParsesTheValidShape()
    {
        const auto config = Configuration::Parse(kValid, kFile);
        Expect(config.has_value(), "the schema-1 shape parses");
        if (!config) return;
        ExpectEq(config->Requirement, std::string("0.2"), "Requirement");
        ExpectEq(config->Root.generic_string(), std::string("C:/consumer"), "Root is sherlock.json's directory");
        ExpectEq(config->Towers.generic_string(), std::string("C:/consumer/towers.json"), "Towers resolves against the root");
        ExpectEq(config->Corpus.Root.generic_string(), std::string("C:/consumer"), "the corpus reads from the root");
        Expect(config->Corpus.Collections.size() == 3 &&
                   config->Corpus.Collections[0].Kind == DocumentIndex::CollectionKind::Evidence &&
                   config->Corpus.Collections[1].Kind == DocumentIndex::CollectionKind::Concept &&
                   config->Corpus.Collections[2].Kind == DocumentIndex::CollectionKind::Code &&
                   config->Corpus.Collections[2].Extensions == std::vector<std::string>{".h", ".cpp"},
               "three collections in declaration order, each with its kind");
        Expect(config->Corpus.Seals && config->Corpus.Seals->Tags == std::vector<std::string>{"BIN", "KIT"} &&
                   config->Corpus.Seals->ImageTag == "BIN",
               "the seal tags and the image tag");
    }

    void GateOptionalKeys()
    {
        const auto config = Configuration::Parse(
            R"({"schema": 1, "sherlock": "0.2", "collections": [{"path": "notes", "kind": "evidence"}]})", kFile);
        Expect(config.has_value() && config->Towers.empty() && !config->Corpus.Seals.has_value(),
               "towers and seals are optional");
    }

    void GateUtf8ByteOrderMark()
    {
        Expect(Configuration::Parse("\xEF\xBB\xBF" + kValid, kFile).has_value(),
               "a sherlock.json a Windows editor saved with a UTF-8 byte order mark parses");
    }

    void GatePathsNormalize()
    {
        const auto trailing  = Configuration::Parse(Replaced(kValid, R"("docs/re")", R"("docs/re/")"), kFile);
        const auto backslash = Configuration::Parse(Replaced(kValid, R"("docs/re")", R"("docs\\re")"), kFile);
        Expect(trailing && trailing->Corpus.Collections[0].Path == "docs/re", "a trailing slash names the same collection");
        Expect(backslash && backslash->Corpus.Collections[0].Path == "docs/re", "a backslash names the same collection");
    }

    void GateRefusals()
    {
        ExpectRefused("{", "not valid JSON", "a syntax error");
        ExpectRefused("[1]", "not a JSON object", "a top-level array");
        ExpectRefused(Replaced(kValid, R"("schema": 1,)", R"("schema": 1, "extra": true,)"), R"(unknown key "extra")", "an unknown top-level key");
        ExpectRefused(Replaced(kValid, R"("kind": "evidence"})", R"("kind": "evidence", "mayBeEmpty": true})"), R"(unknown key "mayBeEmpty")", "an unknown collection key");
        ExpectRefused(Replaced(kValid, R"("imageTag": "BIN")", R"("imageTag": "BIN", "grammar": 2)"), R"(unknown key "grammar")", "an unknown seals key");
        ExpectRefused(Replaced(kValid, R"("kind": "evidence")", R"("kind": "evidance")"), R"("evidance")", "an invalid kind");
        ExpectRefused(Replaced(kValid, R"("schema": 1)", R"("schema": 2)"), R"("schema" is 2)", "another schema");
        ExpectRefused(Replaced(kValid, R"("schema": 1)", R"("schema": "1")"), R"("schema" is "1")", "a schema that is not a number");
        ExpectRefused(Replaced(kValid, R"("sherlock": "0.2", )", ""), R"(missing key "sherlock")", "no version requirement");
        ExpectRefused(Replaced(kValid, kCollections, "[]"), "non-empty array", "no collection");
        ExpectRefused(Replaced(kValid, R"("kind": "evidence"})", R"("kind": "evidence", "extensions": [".md"]})"), "applies to kind code only", "extensions on an evidence collection");
        ExpectRefused(Replaced(kValid, R"(, "extensions": [".h", ".cpp"])", ""), R"(needs "extensions")", "a code collection with no extensions");
        ExpectRefused(Replaced(kValid, R"(".cpp")", R"("cpp")"), "does not start with a dot", "an extension without its dot");
        ExpectRefused(Replaced(kValid, R"(["BIN", "KIT"])", R"(["BIN", "kit"])"), "not an upper-case word", "a lower-case tag");
        ExpectRefused(Replaced(kValid, R"(["BIN", "KIT"])", R"(["BIN", "B.N"])"), R"("B.N")", "a tag carrying a regex metacharacter");
        ExpectRefused(Replaced(kValid, R"(["BIN", "KIT"])", R"(["BIN", "K T"])"), R"("K T")", "a tag carrying a space");
        ExpectRefused(Replaced(kValid, R"(["BIN", "KIT"])", R"(["BIN", 7])"), "not an upper-case word", "a tag that is not a string");
        ExpectRefused(Replaced(kValid, R"(["BIN", "KIT"])", "[]"), R"("tags" is empty)", "no tag");
        ExpectRefused(Replaced(kValid, R"("imageTag": "BIN")", R"("imageTag": "OBS")"), R"(is not one of "tags")", "an image tag that is not declared");
        ExpectRefused(Replaced(kValid, R"("path": "docs/re")", R"("path": "C:/docs/re")"), "not relative", "an absolute collection path");
        ExpectRefused(Replaced(kValid, R"("path": "docs/re")", R"("path": "../outside")"), "leaves sherlock.json's directory", "a collection above the root");
        ExpectRefused(Replaced(kValid, R"("towers": "towers.json")", R"("towers": 7)"), R"("towers" is not a path)", "a towers value that is not a path");
    }

    void GateRequirement()
    {
        using Pair = std::pair<const char*, const char*>;
        for (const auto& [required, running] : std::initializer_list<Pair>{
                 {"0.2", "0.2.0"}, {"0.2", "0.2.7"}, {"0.2.0", "0.2.0"}, {"1.2", "1.3.0"}, {"1.2.3", "1.2.3"}})
        {
            Expect(Configuration::CheckRequirement(required, running).has_value(),
                   std::format("{} accepts Sherlock {}", required, running));
        }
        for (const auto& [required, running] : std::initializer_list<Pair>{
                 {"0.2.1", "0.2.0"}, {"0.3", "0.2.0"}, {"0.1", "0.2.0"}, {"2.0", "1.9.9"}, {"1.4", "1.3.0"}})
        {
            const auto checked = Configuration::CheckRequirement(required, running);
            const auto text    = checked ? std::string("accepted") : checked.error().Format();
            Expect(!checked && checked.error().Level == Foundation::Severity::NotVerified &&
                       text.find(required) != std::string::npos && text.find(running) != std::string::npos,
                   std::format("{} refuses Sherlock {} and states both versions (got: {})", required, running, text));
        }
        for (const char* malformed : {"1", "0.x", "0.2.", "", "0.2.0.1"})
        {
            Expect(!Configuration::CheckRequirement(malformed, "0.2.0").has_value(),
                   std::format("\"{}\" is not a requirement", malformed));
        }
    }

    void GateSha256()
    {
        const auto empty = Configuration::Sha256Hex("");
        const auto abc   = Configuration::Sha256Hex("abc");
        Expect(empty && *empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of no bytes");
        Expect(abc && *abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    }

    void GateDiscover()
    {
        const auto root = std::filesystem::temp_directory_path() / "SherlockDiscoverGate";
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        Write(root / "outer" / "sherlock.json", "{}");
        Write(root / "outer" / "inner" / "sherlock.json", "{}");
        std::filesystem::create_directories(root / "outer" / "inner" / "deep");
        const auto nearest = Configuration::Discover({}, root / "outer" / "inner" / "deep");
        Expect(nearest && *nearest == root / "outer" / "inner" / "sherlock.json",
               "a consumer nested in another reads the nearest sherlock.json walking up");
        const auto here = Configuration::Discover({}, root / "outer");
        Expect(here && *here == root / "outer" / "sherlock.json", "the start directory itself is looked at first");
        const auto named = Configuration::Discover(root / "outer" / "sherlock.json", root / "outer" / "inner");
        Expect(named && *named == root / "outer" / "sherlock.json", "--config wins over the walk");
        const auto missing = Configuration::Discover(root / "nope.json", root);
        Expect(!missing && missing.error().Format().find("--config") != std::string::npos,
               "a --config that names no file is refused, naming --config");
        const auto none = Configuration::Discover({}, std::filesystem::path("Z:/SherlockNoConsumer/deep"));
        Expect(!none && none.error().Format().find("no sherlock.json") != std::string::npos &&
                   none.error().Format().find("--config") != std::string::npos,
               "no sherlock.json up the walk is refused, naming the start and --config");
        std::filesystem::remove_all(root, cleanupError);
    }

    void GateValidateAndLoad()
    {
        const auto root = std::filesystem::temp_directory_path() / "SherlockValidateGate";
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        Write(root / "towers.json", R"({"build": "TEST1", "image": {}})");
        Write(root / "docs" / "re" / "a.md", "# 1 A\n");
        Write(root / "docs" / "concepts" / "c.md", "---\ntitle: C\naliases: []\n---\n");
        Write(root / "Source" / "s.h", "// [BIN] Sample 0x240622d98\n");
        Write(root / "sherlock.json", kValid);

        const auto loaded = Configuration::Load(root / "sherlock.json", "0.2.0");
        Expect(loaded.has_value(), "a consumer whose every path exists and holds files loads" +
                                       (loaded ? std::string() : ": " + loaded.error().Format()));
        const auto sha = Configuration::Sha256Hex(kValid);
        Expect(loaded && sha && loaded->Corpus.ConfigSha256 == *sha, "Load records the SHA-256 of the file's bytes");
        Expect(loaded && loaded->Root.is_absolute(), "Load resolves the root to an absolute path");
        const auto old = Configuration::Load(root / "sherlock.json", "0.1.0");
        Expect(!old && old.error().Format().find("requires Sherlock 0.2 and this is Sherlock 0.1.0") != std::string::npos,
               "Load checks the version requirement");

        const auto parsed = [&](const std::string& json) {
            auto config = Configuration::Parse(json, root / "sherlock.json");
            if (!config)
            {
                std::printf("FAIL: a Validate fixture did not parse: %s\n", config.error().Format().c_str());
                std::exit(1);
            }
            return *config;
        };
        const auto refusedBy = [&](const std::string& json, std::string_view fragment, std::string_view what) {
            const auto checked = Configuration::Validate(parsed(json));
            const auto text    = checked ? std::string("accepted") : checked.error().Format();
            Expect(!checked && text.find(fragment) != std::string::npos, std::string(what) + " -- got: " + text);
        };
        Expect(Configuration::Validate(parsed(kValid)).has_value(), "Validate accepts the complete tree");
        refusedBy(Replaced(kValid, R"("towers": "towers.json")", R"("towers": "missing.json")"), R"("towers" names)",
                  "a towers file that does not exist");
        refusedBy(Replaced(kValid, R"("path": "docs/re")", R"("path": "docs/nope")"), R"(collection "docs/nope")",
                  "a collection directory that does not exist");
        Write(root / "docs" / "empty" / "README.md", "not indexed\n");
        refusedBy(Replaced(kValid, R"("path": "docs/re")", R"("path": "docs/empty")"), "holds no file",
                  "a collection with no file Sherlock reads");
        refusedBy(Replaced(kValid, R"([".h", ".cpp"])", R"([".glsl"])"), R"(collection "Source" holds no file)",
                  "a code collection whose extensions match nothing");
        std::filesystem::remove_all(root, cleanupError);
    }
}

int main()
{
    GateParsesTheValidShape();
    GateOptionalKeys();
    GateUtf8ByteOrderMark();
    GatePathsNormalize();
    GateRefusals();
    GateRequirement();
    GateSha256();
    GateDiscover();
    GateValidateAndLoad();
    return Finish();
}
