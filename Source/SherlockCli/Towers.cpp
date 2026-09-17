// Sherlock — tools/Sherlock/Source/SherlockCli/Towers.cpp
// nlohmann::json read of towers.json's "build" and "image" keys.
#include <SherlockCli/Towers.h>

#include <nlohmann/json.hpp>

#include <fstream>

namespace Sherlock::Cli
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        Expected<nlohmann::json> Load(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "Towers::Load", path.string(),
                            "the file cannot be opened", "check the path");
            }
            try
            {
                nlohmann::json j;
                in >> j;
                return j;
            }
            catch (const std::exception& e)
            {
                return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "Towers::Load", path.string(),
                            "the file is not valid JSON", "check towers.json's syntax", e.what());
            }
        }
    }

    Expected<std::vector<TowerImage>> ReadTowers(const std::filesystem::path& towersJson)
    {
        const auto json = Load(towersJson);
        if (!json)
        {
            return std::unexpected(json.error());
        }
        const auto it = json->find("image");
        if (it == json->end() || !it->is_object())
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "ReadTowers", towersJson.string(),
                        "the file carries no \"image\" object", "check towers.json's shape");
        }
        std::vector<TowerImage> towers;
        for (const auto& [tower, path] : it->items())
        {
            if (path.is_string() && !path.get<std::string>().empty())
            {
                towers.push_back({tower, path.get<std::string>()});
            }
        }
        return towers;
    }

    Expected<std::string> ReadTowersBuild(const std::filesystem::path& towersJson)
    {
        const auto json = Load(towersJson);
        if (!json)
        {
            return std::unexpected(json.error());
        }
        const auto it = json->find("build");
        if (it == json->end() || !it->is_string())
        {
            return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "ReadTowersBuild", towersJson.string(),
                        "the file carries no \"build\" string", "check towers.json's shape");
        }
        return it->get<std::string>();
    }
}
