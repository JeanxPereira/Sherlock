// Sherlock — Source/SherlockCli/include/SherlockCli/LegacyCorpus.h
// The three collections the CLI reads until sherlock.json is wired in (derived).
#pragma once

#include <DocumentIndex/Corpus.h>

namespace Sherlock::Cli
{
    inline DocumentIndex::Corpus LegacyCorpus(const std::filesystem::path& repo)
    {
        using DocumentIndex::CollectionKind;
        DocumentIndex::Corpus corpus;
        corpus.Root        = repo;
        corpus.Collections = {{"docs/re", CollectionKind::Evidence, {}},
                              {"docs/concepts", CollectionKind::Concept, {}},
                              {"Source", CollectionKind::Code, {".h", ".hpp", ".cpp", ".frag", ".vert", ".glsl"}}};
        corpus.Seals = DocumentIndex::SealGrammar{{"BIN", "KIT", "OBS", "API", "INF", "DEMO", "ASSUMPTION"}, "BIN"};
        return corpus;
    }
}
