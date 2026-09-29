// Sherlock — tests/SherlockHarness.h
// Expectations, the exit code, and the corpus paths ctest hands over (derived).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string_view>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

#include <windows.h>

#include <exception>

namespace
{
    // A debug assert or an abort has no dialog to show under ctest, and the default
    // report mode opens one anyway -- the run then waits on a window nobody sees, which
    // reads as a slow test. Both reports go to stderr, so a gate dies loudly instead.
    const int gCrtReportToStderr = []
    {
#ifdef _DEBUG
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        // Windows' own fault box is the other window a headless run cannot answer.
        ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        std::set_terminate([] {
            std::fputs("terminate called: an exception escaped the gate\n", stderr);
            std::fflush(stderr);
            ::TerminateProcess(::GetCurrentProcess(), 3);
        });
        return 0;
    }();

    int gFailures = 0;

    void Expect(bool ok, std::string_view what)
    {
        if (!ok)
        {
            std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
            ++gFailures;
        }
    }

    template <class A, class B>
    void ExpectEq(const A& actual, const B& expected, std::string_view what)
    {
        if (!(actual == expected))
        {
            std::printf("FAIL: %.*s: got %s, expected %s\n", static_cast<int>(what.size()), what.data(),
                        std::format("{}", actual).c_str(), std::format("{}", expected).c_str());
            ++gFailures;
        }
    }

    // A missing environment variable is a gate that could not run: exit 2, never a pass.
    std::filesystem::path RequiredDirectory(const char* name)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || !std::filesystem::is_directory(value))
        {
            std::printf("NOT VERIFIED: %s is not set to an existing directory\n", name);
            std::exit(2);
        }
        return value;
    }

    std::filesystem::path CacheDirectory() { return RequiredDirectory("SHERLOCK_CACHE"); }

    std::filesystem::path StoreDirectory() { return RequiredDirectory("SHERLOCK_STORE"); }

    int Finish()
    {
        if (gFailures == 0)
        {
            std::printf("OK\n");
            return 0;
        }
        std::printf("%d failure(s)\n", gFailures);
        return 1;
    }
}
