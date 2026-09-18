// Sherlock — tools/Sherlock/Source/HexRaysExport/include/HexRaysExport/Store.h
// Layer 2's own database: one decompilation row per function, and the coverage a zero reports (derived).
#pragma once

#include <Foundation/Diagnostic.h>
#include <Store/Database.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Sherlock::HexRaysExport
{
    using Foundation::Expected;

    // Independent of Store::kSchemaVersion. Layer 1 costs the whole cache to rebuild, and a
    // decompilation-schema change is not a reason to pay that -- so layer 2 carries its own
    // version in its own file, the way the document layer already does.
    inline constexpr int kHexRaysSchemaVersion = 1;

    enum class Status
    {
        Ok,
        Timeout,
        Failed,
    };

    std::string_view ToText(Status status);
    std::optional<Status> FromText(std::string_view text);

    struct DecompilationRow
    {
        std::uint64_t Function = 0;  // cache VA, the same number a laudo cites
        std::string   Pseudocode;    // plain text here, a zstd blob on disk
        std::uint32_t Lines = 0;
        Status        State = Status::Ok;
        std::string   Reason;  // empty unless State is not Ok
        double        Seconds = 0;
    };

    // IDA's name for an address, recorded only where it differs from the symbol layer 1 read.
    struct NameRow
    {
        std::uint64_t Address = 0;
        std::string   Name;
    };

    struct Coverage
    {
        std::uint64_t Decompiled = 0;
        std::uint64_t Attempted  = 0;
    };

    std::filesystem::path StorePath(const std::filesystem::path& storeDir, std::string_view imageName);

    Expected<void> CreateStore(Store::Database& db, std::string_view imagePath, std::string_view build,
                               std::string_view idaVersion);
    Expected<void> CheckStoreSchema(Store::Database& db);

    Expected<void> WriteRows(Store::Database& db, const std::vector<DecompilationRow>& rows,
                             const std::vector<NameRow>& names);

    Expected<std::optional<DecompilationRow>> ReadFunction(Store::Database& db, std::uint64_t address);
    Expected<Coverage>                        ReadCoverage(Store::Database& db);
}
