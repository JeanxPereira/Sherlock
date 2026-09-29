// Sherlock — Source/DocumentIndex/include/DocumentIndex/FileStamp.h
// A cheap per-file stat (Size/MTime, no content read), used by document staleness checks (derived).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace Sherlock::DocumentIndex
{
    struct FileStamp
    {
        std::uintmax_t Size = 0;
        std::int64_t MTime = 0;
    };

    std::optional<FileStamp> StatFile(const std::filesystem::path& file);
}
