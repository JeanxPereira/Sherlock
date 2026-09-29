// Sherlock — Source/SherlockCli/include/SherlockCli/HexRays.h
// `build hexrays`: the parent that spawns one worker per image and owns the catalog (derived).
#pragma once

#include <SherlockCli/Verdict.h>

namespace Sherlock::Cli
{
    struct Invocation;

    struct BuildHexRaysResult
    {
        Verdict Report;
        // 0 every selected image is exported, 1 an image failed, 2 the run could not be made.
        int ExitCode = 2;
    };

    // The verdict travels back rather than being printed here: the JSON shape lives in one place.
    BuildHexRaysResult BuildHexRays(const Invocation& invocation);
}
