// Sherlock — Source/HexRaysExport/include/HexRaysExport/Store.h
// Layer 2's own database: one decompilation row per function, and the coverage a zero reports (derived).
#pragma once

#include <Foundation/Diagnostic.h>
#include <Store/Database.h>

#include <cstdint>
#include <filesystem>
#include <functional>
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

    // What a too-big row carries in its Reason, written from here rather than copied from the
    // decompiler's own wording: the count of them is read back out of a finished store with an
    // equality query, and IDA's description text is not ours to hold stable.
    inline constexpr std::string_view kTooBigReason = "MERR_FUNCSIZE: too big for the decompiler";

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
        std::uint64_t TooBig     = 0;
        std::uint64_t Lines      = 0;
    };

    std::filesystem::path StorePath(const std::filesystem::path& storeDir, std::string_view imageName);

    Expected<void> CreateStore(Store::Database& db, std::string_view imagePath, std::string_view build,
                               std::string_view idaVersion);
    Expected<void> CheckStoreSchema(Store::Database& db);

    Expected<void> WriteRows(Store::Database& db, const std::vector<DecompilationRow>& rows,
                             const std::vector<NameRow>& names);

    // A store is written in batches, so one that exists says nothing about whether every function
    // in the image was attempted. Complete is what says it, and MarkComplete is the last write a
    // finished run makes.
    Expected<void> MarkComplete(Store::Database& db);

    // A store with no Complete key was written before layer 2 wrote in batches, when the single
    // write at the end made existence and completeness the same fact. Those stores are complete,
    // and reading the absent key as "unfinished" would re-decompile every one of them.
    Expected<bool> IsComplete(Store::Database& db);

    // The addresses a partial store already carries, so an interrupted run resumes at the
    // function it did not reach instead of at the first one.
    Expected<std::vector<std::uint64_t>> ReadStoredFunctions(Store::Database& db);

    Expected<std::optional<DecompilationRow>> ReadFunction(Store::Database& db, std::uint64_t address);
    Expected<Coverage>                        ReadCoverage(Store::Database& db);

    // Every decompiled function in the store, one at a time. A text search over an image must not
    // hold the whole of its pseudocode at once -- AppKit's is tens of megabytes decompressed.
    // The visitor stops the walk by returning false.
    Expected<void> ForEachPseudocode(Store::Database& db,
                                     const std::function<bool(std::uint64_t, std::string_view)>& visit);

    // Functions the store holds with no pseudocode, because the decompiler refused them. A text
    // search cannot see inside these, and a search reporting no hits has to say how many there
    // are: otherwise its zero claims something it never looked at.
    Expected<std::uint64_t> CountWithoutPseudocode(Store::Database& db);
}
