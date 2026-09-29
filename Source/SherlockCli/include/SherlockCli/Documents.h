// Sherlock — Source/SherlockCli/include/SherlockCli/Documents.h
// find and laudo answer from Documents.db, the derived document layer.
#pragma once

#include <SherlockCli/Verdict.h>

#include <string_view>

namespace Sherlock::Cli
{
    struct QueryEnvironment;

    Verdict RunFind(const QueryEnvironment& env, std::string_view text);
    Verdict RunLaudo(const QueryEnvironment& env, std::string_view slug, std::string_view section);
}
