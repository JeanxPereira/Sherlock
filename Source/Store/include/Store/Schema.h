// Sherlock — tools/Sherlock/Source/Store/include/Store/Schema.h
// The catalog and image-store schemas, and the version a store must carry to be read (derived).
#pragma once

#include <Store/Database.h>

#include <string_view>

namespace Sherlock::Store
{
    // Moves only when a table or a column changes; independent of the tool's own version.
    inline constexpr int kSchemaVersion = 1;

    Expected<void> CreateCatalog(Database& db, std::string_view build, std::string_view cacheUuid);
    Expected<void> CreateImageStore(Database& db, std::string_view imagePath);
    Expected<void> CreateImageIndexes(Database& db);
    Expected<void> CheckSchema(Database& db, std::string_view kind);
}
