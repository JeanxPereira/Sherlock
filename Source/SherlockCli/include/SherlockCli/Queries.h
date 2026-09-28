// Sherlock — tools/Sherlock/Source/SherlockCli/include/SherlockCli/Queries.h
// q, callers, calls, refs, status -- each opens the image stores it needs and returns one Verdict (derived).
#pragma once

#include <DyldSharedCache/Cache.h>
#include <Foundation/Diagnostic.h>
#include <SherlockCli/Verdict.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <string>
#include <vector>

namespace Sherlock::Cli
{
    struct QueryEnvironment
    {
        std::filesystem::path Store;
        std::filesystem::path Documents;
        std::filesystem::path Repo;
        bool                  Full = false;
        bool                  Json = false;
        std::vector<std::string>* Output = nullptr;
        std::function<Foundation::Expected<void>(std::string_view)> PlainTextSink;
    };

    Foundation::Expected<void> PrintHeader(const QueryEnvironment& env, std::string_view build);

    // q's layer 3: the doc sections citing a resolved address and the seals sealed at it, or the
    // one line "layer 3: not built" when Documents.db is absent or schema-mismatched (decision 4).
    // Needs only an address, never a DyldSharedCache::Cache, so a gate can cover it without one.
    void PrintDocumentLayer(const QueryEnvironment& env, std::uint64_t address);

    Verdict RunQuery(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::string_view target);
    Verdict RunCallers(const QueryEnvironment& env, std::uint64_t address);
    Verdict RunCalls(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t address);
    // vcall: the arm64e PAC virtual-dispatch family stored at a blraa/braa site (Facts::VirtualCall).
    Verdict RunVirtualCall(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t site);
    Verdict RunRefs(const DyldSharedCache::Cache& cache, const QueryEnvironment& env, std::uint64_t address,
                    std::uint64_t end);
    // fn: layer 2's pseudocode for the function containing the address, or layer 1's disassembly
    // with --asm. A cache is needed only for --asm, so the layer-2 path costs no cache open.
    Verdict RunFunction(const QueryEnvironment& env, const DyldSharedCache::Cache* cache,
                        std::uint64_t address, bool disassemble);

    // grep: one pattern over layer 2's pseudocode, across every image or the ones `images` names.
    // It answers the question layer 2 exists for and grep on disk cannot reach -- "which function
    // mentions this" -- and it is the one query whose ZERO is dangerous, because an image with no
    // layer 2 is silent in exactly the way an image without the pattern is. Every image it could
    // not read is named in the output, and so is the number of functions the decompiler refused.
    Verdict RunGrep(const QueryEnvironment& env, std::string_view pattern,
                    const std::vector<std::string>& images, bool ignoreCase, std::size_t limit);

    Verdict RunStatus(const QueryEnvironment& env);
}
