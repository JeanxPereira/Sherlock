// Sherlock — Source/DocumentIndex/FileStamp.cpp
#include <DocumentIndex/FileStamp.h>

#include <system_error>

namespace Sherlock::DocumentIndex
{
    std::optional<FileStamp> StatFile(const std::filesystem::path& file)
    {
        std::error_code sizeError, timeError;
        const auto size = std::filesystem::file_size(file, sizeError);
        const auto time = std::filesystem::last_write_time(file, timeError);
        if (sizeError || timeError)
        {
            return std::nullopt;
        }
        return FileStamp{size, time.time_since_epoch().count()};
    }
}
