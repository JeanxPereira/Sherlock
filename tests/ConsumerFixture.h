// Sherlock — tests/ConsumerFixture.h
// A consumer configuration over the fixture corpus, for the gates that run CLI code in-process (derived).
#pragma once

#include "CorpusFixture.h"

#include <Configuration/Config.h>

namespace
{
    Sherlock::Configuration::Config FixtureConsumer(const std::filesystem::path& root)
    {
        Sherlock::Configuration::Config config;
        config.File        = root / "sherlock.json";
        config.Root        = root;
        config.Requirement = "0.2";
        config.Corpus      = FixtureCorpus(root);
        return config;
    }
}
