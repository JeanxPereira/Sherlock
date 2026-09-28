// Sherlock — tests/Sherlock/HexRaysStoreGates.cpp
// Layer 2's own schema version, the compressed round trip, and the coverage a zero reports.
#include <HexRaysExport/Store.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include "SherlockHarness.h"

#include <string>

using namespace Sherlock;

namespace
{
    std::filesystem::path Fresh(const char* name)
    {
        const auto path = std::filesystem::temp_directory_path() / name;
        for (const char* suffix : {"", "-wal", "-shm"})
        {
            std::filesystem::remove(path.string() + suffix);
        }
        return path;
    }

    // Layer 1 costs the whole cache to rebuild. If a layer-2 change ever bumped the image store's
    // version, every Facts store on disk would be refused at once -- so the two versions are held
    // apart here, and this gate fails the day someone reaches for the wrong constant.
    void GateVersionsAreIndependent()
    {
        static_assert(HexRaysExport::kHexRaysSchemaVersion == 1);
        static_assert(Store::kSchemaVersion == 2);
        static_assert(Store::kDocumentsSchemaVersion == 1);
        Expect(&HexRaysExport::kHexRaysSchemaVersion != &Store::kSchemaVersion,
               "layer 2's schema version is its own constant, not layer 1's");
    }

    void GateRoundTrip()
    {
        const auto path = Fresh("SherlockHexRaysStoreGate.db");
        auto db = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "the layer-2 store opens read-write");
        Expect(HexRaysExport::CreateStore(*db, "/System/Library/PrivateFrameworks/DesignLibrary.framework/"
                                               "Versions/A/DesignLibrary",
                                          "26A5416b", "9.2.250904")
                   .has_value(),
               "the layer-2 schema is created");
        Expect(HexRaysExport::CheckStoreSchema(*db).has_value(), "the store answers its own kind and version");

        // Real pseudocode carries braces, tabs and long runs of spaces; a line that compresses to
        // nothing would not exercise the blob path the way the corpus will.
        std::string pseudocode = "void __fastcall sub_240492540(__int64 a1)\n{\n";
        for (int i = 0; i < 200; ++i)
        {
            pseudocode += "  v" + std::to_string(i) + " = *(_QWORD *)(a1 + " + std::to_string(i * 8) + ");\n";
        }
        pseudocode += "}\n";

        const std::vector<HexRaysExport::DecompilationRow> rows{
            {0x240492540ull, pseudocode, 203, HexRaysExport::Status::Ok, "", 0.012},
            {0x2404925c0ull, "", 0, HexRaysExport::Status::Failed, "function exceeds MAX_FUNCSIZE", 0.0},
            {0x240492640ull, "", 0, HexRaysExport::Status::Timeout, "60 s budget", 60.0},
        };
        Expect(HexRaysExport::WriteRows(*db, rows, {{0x240492540ull, "DesignLibrary.layerTable"}}).has_value(),
               "the rows are written");

        auto got = HexRaysExport::ReadFunction(*db, 0x240492540ull);
        Expect(got.has_value() && got->has_value(), "the decompiled function reads back");
        if (got && *got)
        {
            ExpectEq((*got)->Pseudocode, pseudocode, "the pseudocode survives the zstd round trip");
            ExpectEq((*got)->Lines, 203u, "the line count survives");
            Expect((*got)->State == HexRaysExport::Status::Ok, "the status survives");
            ExpectEq((*got)->Seconds, 0.012, "the elapsed seconds survive");
        }

        // A blob is not text: compressed bytes carry nulls, and a TEXT binding would have cut the
        // payload at the first one and still read back as a shorter, valid-looking string.
        const auto stored = db->ScalarInt("SELECT length(Pseudocode) FROM Decompilation WHERE Status = 'Ok'");
        Expect(stored.has_value() && *stored > 0, "the blob column holds bytes");
        Expect(stored.has_value() && static_cast<std::size_t>(*stored) < pseudocode.size(),
               "the stored blob is smaller than the text it came from");

        auto failed = HexRaysExport::ReadFunction(*db, 0x2404925c0ull);
        Expect(failed.has_value() && failed->has_value(), "a function that did not decompile still has a row");
        if (failed && *failed)
        {
            Expect((*failed)->State == HexRaysExport::Status::Failed, "its status is Failed");
            ExpectEq((*failed)->Reason, std::string("function exceeds MAX_FUNCSIZE"), "its reason is kept");
        }

        auto missing = HexRaysExport::ReadFunction(*db, 0xdeadbeefull);
        Expect(missing.has_value() && !missing->has_value(), "an address with no row is empty, not an error");

        auto coverage = HexRaysExport::ReadCoverage(*db);
        Expect(coverage.has_value(), "coverage reads");
        if (coverage)
        {
            ExpectEq(coverage->Decompiled, std::uint64_t{1}, "coverage counts the rows that decompiled");
            ExpectEq(coverage->Attempted, std::uint64_t{3}, "coverage counts every row attempted");
        }
    }

    void GateSchemaRefusal()
    {
        const auto path = Fresh("SherlockHexRaysWrongKind.db");
        auto db = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "a second database opens");
        Expect(Store::CreateImageStore(*db, "/usr/lib/test.dylib", "test-cache").has_value(),
               "a layer-1 image store is created in it");
        // Pointing a layer-2 read at a layer-1 store must refuse: both carry a Meta table with a
        // SchemaVersion of 1, so only the Kind tells them apart.
        Expect(!HexRaysExport::CheckStoreSchema(*db).has_value(),
               "a layer-1 store is refused as a layer-2 store");
    }
}

int main()
{
    GateVersionsAreIndependent();
    GateRoundTrip();
    GateSchemaRefusal();
    return Finish();
}
