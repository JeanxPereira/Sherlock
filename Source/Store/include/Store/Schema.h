// Sherlock — tools/Sherlock/Source/Store/include/Store/Schema.h
// The catalog and image-store schemas, and the version a store must carry to be read (derived).
#pragma once

#include <Store/Database.h>

#include <string_view>

namespace Sherlock::Store
{
    // Moves only when a table or a column changes; independent of the tool's own version.
    inline constexpr int kSchemaVersion = 1;
    // Independent of kSchemaVersion (layer 1/2's catalog and image stores): a change to the
    // Documents.db tables must never force a rebuild of the already-built, expensive Facts
    // stores, and vice versa -- each layer's schema moves on its own decoding, not the other's.
    inline constexpr int kDocumentsSchemaVersion = 1;

    Expected<void> CreateCatalog(Database& db, std::string_view build, std::string_view cacheUuid);
    Expected<void> CreateImageStore(Database& db, std::string_view imagePath, std::string_view cacheUuid = {});
    Expected<void> CreateDocumentsStore(Database& db);
    Expected<void> CreateDocumentIndexes(Database& db);
    Expected<void> CheckDocumentsSchema(Database& db);
    Expected<std::string> ReadMeta(Database& db, std::string_view key);
    Expected<void> CreateImageIndexes(Database& db);
    Expected<void> CheckSchema(Database& db, std::string_view kind, int expectedVersion = kSchemaVersion);
}
