// Sherlock — tests/CorpusFixture.h
// The three-collection corpus the fixture repositories in these gates are laid out as (derived).
#pragma once

#include <DocumentIndex/Corpus.h>
#include <DocumentIndex/SealExtractor.h>

namespace
{
    Sherlock::DocumentIndex::Corpus FixtureCorpus(const std::filesystem::path& root)
    {
        using Sherlock::DocumentIndex::CollectionKind;
        Sherlock::DocumentIndex::Corpus corpus;
        corpus.Root        = root;
        corpus.Collections = {{"docs/re", CollectionKind::Evidence, {}},
                              {"docs/concepts", CollectionKind::Concept, {}},
                              {"Source", CollectionKind::Code, {".h", ".hpp", ".cpp", ".frag", ".vert", ".glsl"}}};
        corpus.Seals = Sherlock::DocumentIndex::SealGrammar{{"BIN", "KIT", "OBS", "API", "INF", "DEMO", "ASSUMPTION"}, "BIN"};
        return corpus;
    }

    const Sherlock::DocumentIndex::SealMatcher& FixtureSeals()
    {
        static const Sherlock::DocumentIndex::SealMatcher matcher(*FixtureCorpus({}).Seals);
        return matcher;
    }
}
