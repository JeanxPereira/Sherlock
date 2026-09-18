// Sherlock — tools/Sherlock/Source/DocumentIndex/include/DocumentIndex/GitHead.h
// The current HEAD SHA, read from .git without spawning git (derived).
#pragma once

#include <Foundation/Diagnostic.h>

#include <filesystem>
#include <string>

namespace Sherlock::DocumentIndex
{
    Foundation::Expected<std::string> ReadCurrentHead(const std::filesystem::path& repoRoot);
}
