// Sherlock — tests/VirtualCallGates.cpp
// The VirtualCall table's own NOT NULL/CHECK refusals, and the arm64e PAC vtable dispatch
// resolver against QuartzCore's real ImagingNode::render dispatch (derived).
#include <DyldSharedCache/Cache.h>
#include <Facts/Disassembler.h>
#include <Facts/ImageFacts.h>
#include <Facts/VirtualCall.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include <array>
#include <cstddef>
#include <set>

#include "SherlockHarness.h"

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

    // A complete row, as INSERT-ready SQL literals -- one column at a time is swapped for NULL
    // by the caller to prove that column's own NOT NULL constraint bites.
    struct Column
    {
        const char* Name;
        const char* Value; // a valid SQL literal for this column
    };

    constexpr Column kCompleteRow[] = {
        {"Image", "'/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore'"},
        {"Site", "0x18b034994"},
        {"Instruction", "'blraa x8, x16'"},
        {"SlotOffset", "0x78"},
        {"Discriminator", "0x6779"},
        {"Candidate", "0x18b265868"},
        {"CandidateSymbol", "'__ZN2CA3OGL7SDFNode5applyEfPPNS0_7SurfaceEPf'"},
        {"Resolver", "'vcall-pac-v1'"},
    };

    std::string InsertSql(int nullColumn)
    {
        std::string columns, values;
        for (int i = 0; i < static_cast<int>(std::size(kCompleteRow)); ++i)
        {
            if (i > 0)
            {
                columns += ", ";
                values += ", ";
            }
            columns += kCompleteRow[i].Name;
            values += (i == nullColumn) ? "NULL" : kCompleteRow[i].Value;
        }
        return "INSERT INTO VirtualCall(" + columns + ") VALUES(" + values + ")";
    }

    bool RefusedAsConstraint(const Foundation::Expected<void>& result, std::string_view needle)
    {
        return !result.has_value() && result.error().Code == Foundation::DiagnosticCode::Database &&
               result.error().System.find(needle) != std::string::npos;
    }

    void GateVirtualCallNotNullRefusesEachColumn()
    {
        const auto path = Fresh("SherlockVirtualCallNotNullGate.db");
        auto        db  = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "the VirtualCall NOT NULL gate database opens");
        if (!db) return;
        Expect(Store::CreateImageStore(*db, "/x", "test-cache").has_value(), "the image schema (with VirtualCall) is created");

        for (int column = 0; column < static_cast<int>(std::size(kCompleteRow)); ++column)
        {
            const auto result = db->Execute(InsertSql(column));
            Expect(RefusedAsConstraint(result, "NOT NULL constraint failed"),
                   std::string("a NULL ") + kCompleteRow[column].Name + " is refused, never stored");
        }
        Expect(db->ScalarInt("SELECT count(*) FROM VirtualCall").value_or(-1) == 0,
               "not one of the incomplete rows above was stored");

        Expect(db->Execute(InsertSql(-1)).has_value(), "a row with every evidence column present is accepted");
        Expect(db->ScalarInt("SELECT count(*) FROM VirtualCall").value_or(-1) == 1,
               "the complete row is the only row stored");
    }

    void GateVirtualCallCheckConstraints()
    {
        const auto path = Fresh("SherlockVirtualCallCheckGate.db");
        auto        db  = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "the VirtualCall CHECK gate database opens");
        if (!db) return;
        Expect(Store::CreateImageStore(*db, "/x", "test-cache").has_value(), "the image schema is created");

        Expect(RefusedAsConstraint(
                   db->Execute("INSERT INTO VirtualCall VALUES('/x', 1, 'blraa x8,x16', 8, 0, 2, 's', 'vcall-pac-v1')"),
                   "CHECK constraint failed"),
               "discriminator 0 (outside 1..0xFFFF) is refused");
        Expect(RefusedAsConstraint(
                   db->Execute(
                       "INSERT INTO VirtualCall VALUES('/x', 1, 'blraa x8,x16', 8, 0x10000, 2, 's', 'vcall-pac-v1')"),
                   "CHECK constraint failed"),
               "discriminator 0x10000 (outside 1..0xFFFF) is refused");
        Expect(RefusedAsConstraint(
                   db->Execute("INSERT INTO VirtualCall VALUES('/x', 1, 'blraa x8,x16', 4, 1, 2, 's', 'vcall-pac-v1')"),
                   "CHECK constraint failed"),
               "a slot offset that is not a multiple of 8 is refused");
        Expect(db->Execute("INSERT INTO VirtualCall VALUES('/x', 1, 'blraa x8,x16', 8, 1, 2, 's', 'vcall-pac-v1')")
                   .has_value(),
               "discriminator 1 and slot offset 8 (both at their own boundary) are accepted");
        Expect(db->ScalarInt("SELECT count(*) FROM VirtualCall").value_or(-1) == 1,
               "only the one boundary-valid row is stored");
    }

    // The site ImagingNode::render (0x18b034410) dispatches through: the known control this
    // resolver was cross-checked against (References/scripts/vcall.py's module docstring).
    constexpr std::uint64_t kSitePositive       = 0x18b034994;
    constexpr std::uint64_t kApplySdf           = 0x18b265868;
    constexpr std::uint64_t kApplyImaging       = 0x18b2602bc;
    constexpr std::uint64_t kExpectedSlot       = 0x78;
    constexpr std::uint64_t kExpectedDiscriminator = 0x6779;
    // A direct bl in the same neighbourhood -- never a virtual dispatch, so it must produce no row.
    constexpr std::uint64_t kSiteNegativeDirectBl = 0x18b2669c4;
    // vcall.py's own self-test negative 2 (inside CA::WindowServer::Server::render_display_to_
    // target): a real blraa (slot=0x8, D=0x5268) whose discriminator matches no __ZTV family
    // anywhere in QuartzCore's own symbol table -- an honest EMPTY, not a guess, so no row either.
    constexpr std::uint64_t kSiteNegativeEmptyFamily = 0x18b2ec880;

    void GateDiscriminatorSelfCheck()
    {
        // Cross-checked once in vcall.py's own module docstring against a real binary fact.
        ExpectEq(Facts::PtrauthStringDiscriminator("_ZN2CA3OGL11ImagingNode5applyEfPPNS0_7SurfaceEPf"),
                 std::uint64_t{0x6779}, "the ptrauth string discriminator reproduces the known D for ImagingNode::apply");
    }

    // A register-offset ldr (`ldr x8, [x16, x9]`) must never be read as a slot dereference: the
    // offset field has no `#imm`, so vcall.py's _LDR_RE refuses to match it and falls through to
    // the generic dest-invalidation instead of a fabricated offset -- the bug this gate closes let
    // the tracker read `addr_of[base] + 0` for exactly this instruction shape. Each word below is
    // hand-encoded and was verified against capstone directly (`ldr x8, [x16, x9]`,
    // `movk x16, #0x1234, lsl #48`, and the known blraa word from vcall.py's own module docstring)
    // before being trusted here.
    void GateRegisterOffsetLdrNeverFabricatesSlot()
    {
        constexpr std::array<std::byte, 12> kCode = {
            std::byte{0x08}, std::byte{0x6a}, std::byte{0x69}, std::byte{0xf8}, // ldr x8, [x16, x9]
            std::byte{0x90}, std::byte{0x46}, std::byte{0xe2}, std::byte{0xf2}, // movk x16, #0x1234, lsl #48
            std::byte{0x10}, std::byte{0x09}, std::byte{0x3f}, std::byte{0xd7}, // blraa x8, x16
        };

        auto disassembler = Facts::Disassembler::Create();
        Expect(disassembler.has_value(), "the synthetic-code disassembler opens");
        if (!disassembler)
        {
            return;
        }
        constexpr std::uint64_t kWindowStart = 0x1000;
        constexpr std::uint64_t kSite        = kWindowStart + 8; // the blraa
        const auto pattern = Facts::ExtractVirtualCallPatternFromCode(*disassembler, kCode, kWindowStart, kSite);
        Expect(pattern.has_value(), "the synthetic-code pattern extraction runs");
        if (!pattern)
        {
            return;
        }
        Expect(!pattern->has_value(), "a register-offset ldr upstream of blraa is refused, never a fabricated slot=0");
    }

    void GateQuartzCoreVirtualCallParity(const DyldSharedCache::Cache& cache, Facts::Disassembler& disassembler)
    {
        const auto found = cache.FindImage("QuartzCore");
        Expect(found.has_value(), "QuartzCore resolves for the virtual-call parity gate");
        if (!found)
        {
            return;
        }
        const auto image = Facts::ExtractImage(cache, *(*found), disassembler);
        Expect(image.has_value(), "ExtractImage runs over QuartzCore");
        if (!image)
        {
            return;
        }

        std::set<std::uint64_t> candidatesAtSite;
        bool                    sawDirectBlRow     = false;
        bool                    sawEmptyFamilyRow  = false;
        for (const auto& row : image->VirtualCalls)
        {
            if (row.Site == kSitePositive)
            {
                candidatesAtSite.insert(row.Candidate);
                ExpectEq(row.SlotOffset, kExpectedSlot, "ImagingNode::render's dispatch decodes the known slot");
                ExpectEq(row.Discriminator, kExpectedDiscriminator,
                         "ImagingNode::render's dispatch decodes the known discriminator");
                Expect(row.Instruction.find("blraa") != std::string::npos,
                       "the stored instruction text names the blraa that dispatches this site");
            }
            if (row.Site == kSiteNegativeDirectBl)
            {
                sawDirectBlRow = true;
            }
            if (row.Site == kSiteNegativeEmptyFamily)
            {
                sawEmptyFamilyRow = true;
            }
        }
        // Membership, not an exact count: the signature-suffix match is the documented
        // over-approximation vcall.py's own module docstring calls the CONVERSE false-positive
        // risk (CA::OGL::Node has many `apply(float, Surface**, float*)` overrides sharing this
        // slot), and vcall.py's own self-test asserts membership for the same reason, never a size.
        Expect(candidatesAtSite.contains(kApplySdf), "the family at 0x18b034994 contains SDFNode::apply");
        Expect(candidatesAtSite.contains(kApplyImaging), "the family at 0x18b034994 contains ImagingNode::apply");
        Expect(!sawDirectBlRow, "the direct bl at 0x18b2669c4 produces no VirtualCall row");
        Expect(!sawEmptyFamilyRow,
               "the real blraa at 0x18b2ec880, whose discriminator matches no __ZTV family, produces no row");
    }

    void GateVirtualCallWriteRoundTrip(const DyldSharedCache::Cache& cache, Facts::Disassembler& disassembler)
    {
        const auto found = cache.FindImage("QuartzCore");
        if (!found)
        {
            return;
        }
        const auto image = Facts::ExtractImage(cache, *(*found), disassembler);
        if (!image)
        {
            return;
        }
        const auto path = Fresh("SherlockVirtualCallRoundTrip.db");
        auto        db  = Store::Database::Open(path, Store::Database::Mode::ReadWrite);
        Expect(db.has_value(), "the round-trip database opens");
        if (!db) return;
        Expect(Store::CreateImageStore(*db, image->Path, "test-cache").has_value(), "the image schema is created");
        Expect(Facts::WriteImageFacts(*db, *image, [](std::string_view) { return std::nullopt; }).has_value(),
               "WriteImageFacts writes the VirtualCall rows it extracted");
        ExpectEq(db->ScalarInt("SELECT count(*) FROM VirtualCall").value(),
                 static_cast<std::int64_t>(image->VirtualCalls.size()), "VirtualCall row count matches");
        ExpectEq(db->ScalarInt("SELECT count(*) FROM VirtualCall WHERE Resolver != 'vcall-pac-v1'").value(),
                 std::int64_t{0}, "every stored row names the resolver that produced it");
    }
}

int main()
{
    try
    {
        GateVirtualCallNotNullRefusesEachColumn();
        GateVirtualCallCheckConstraints();
        GateDiscriminatorSelfCheck();
        GateRegisterOffsetLdrNeverFabricatesSlot();
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: an exception escaped a storage gate: %s\n", e.what());
        return 1;
    }

    auto cache = DyldSharedCache::Cache::Open(CacheDirectory());
    if (!cache)
    {
        std::printf("NOT VERIFIED: %s\n", cache.error().Format().c_str());
        return 2;
    }
    auto disassembler = Facts::Disassembler::Create();
    if (!disassembler)
    {
        std::printf("NOT VERIFIED: %s\n", disassembler.error().Format().c_str());
        return 2;
    }
    try
    {
        GateQuartzCoreVirtualCallParity(*cache, *disassembler);
        GateVirtualCallWriteRoundTrip(*cache, *disassembler);
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: an exception escaped a parity gate: %s\n", e.what());
        return 1;
    }
    return Finish();
}
