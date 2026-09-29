// Sherlock — Source/SherlockCli/include/SherlockCli/Towers.h
// towers.json's build id and image map, turned into build-order input (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <filesystem>
#include <string>
#include <vector>

namespace Sherlock::Cli
{
    using Foundation::Expected;

    struct TowerImage
    {
        std::string Tower;
        std::string Path;
    };

    Expected<std::vector<TowerImage>> ReadTowers(const std::filesystem::path& towersJson);
    Expected<std::string>             ReadTowersBuild(const std::filesystem::path& towersJson);
}
