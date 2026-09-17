// Sherlock — tests/Sherlock/CliGates.cpp
// Argument parsing, towers.json reading, and the demangler against the measured Swift name.
#include <SherlockCli/Arguments.h>
#include <SherlockCli/Demangler.h>
#include <SherlockCli/Towers.h>

#include "SherlockHarness.h"

using namespace Sherlock;
using Foundation::DiagnosticCode;

namespace
{
    std::filesystem::path RequiredFile(const char* name)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || !std::filesystem::is_regular_file(value))
        {
            std::printf("NOT VERIFIED: %s is not set to an existing file\n", name);
            std::exit(2);
        }
        return value;
    }

    void GateParseArguments()
    {
        {
            const char*     argv[] = {"Sherlock", "build", "facts", "--towers", "--resume", "--workers", "4"};
            const auto invocation = Cli::ParseArguments(7, const_cast<char**>(argv));
            Expect(invocation.has_value(), "build facts --towers --resume --workers 4 parses");
            if (invocation)
            {
                ExpectEq(invocation->Command, std::string("build"), "Command");
                Expect(!invocation->Positional.empty() && invocation->Positional.front() == "facts", "Positional[0]");
                Expect(invocation->AllTowers, "AllTowers");
                Expect(invocation->Resume, "Resume");
                ExpectEq(invocation->Workers, 4u, "Workers");
            }
        }
        {
            const char*     argv[] = {"Sherlock", "refs", "0x29f60f388", "--to", "0x29f60f480"};
            const auto invocation = Cli::ParseArguments(5, const_cast<char**>(argv));
            Expect(invocation.has_value(), "refs <addr> --to <addr> parses");
            if (invocation)
            {
                ExpectEq(invocation->Command, std::string("refs"), "Command");
                Expect(!invocation->Positional.empty() && invocation->Positional.front() == "0x29f60f388", "Positional[0]");
                Expect(invocation->To.has_value() && *invocation->To == std::uint64_t{0x29f60f480}, "To");
            }
        }
        {
            const char*     argv[] = {"Sherlock", "q", "0x1", "--not-a-flag"};
            const auto invocation = Cli::ParseArguments(4, const_cast<char**>(argv));
            Expect(!invocation.has_value() && invocation.error().Code == DiagnosticCode::Usage,
                   "an unknown flag is a Usage diagnostic");
        }
        {
            const char*     argv[] = {"Sherlock", "--version"};
            const auto invocation = Cli::ParseArguments(2, const_cast<char**>(argv));
            Expect(invocation.has_value() && invocation->Command == "version", "--version is its own command");
        }
    }

    void GateTowers()
    {
        const auto path = RequiredFile("SHERLOCK_TOWERS");
        const auto build = Cli::ReadTowersBuild(path);
        Expect(build.has_value() && !build->empty(), "ReadTowersBuild reads the build field");
        const auto towers = Cli::ReadTowers(path);
        Expect(towers.has_value(), "ReadTowers reads the image map");
        if (towers)
        {
            bool sawPlatform = false;
            for (const auto& tower : *towers)
            {
                if (tower.Tower == "Platform")
                {
                    sawPlatform = true;
                }
            }
            Expect(!sawPlatform, "Platform has no install path and is not a tower image entry");
            Expect(!towers->empty(), "at least one tower carries an install path");
        }
    }

    void GateDemangler()
    {
        auto demangler = Cli::Demangler::Load(Cli::DefaultDemanglerPath());
        if (!demangler)
        {
            std::printf("demangler unavailable at %s -- Demangle stays untested on this host, not failed\n",
                       Cli::DefaultDemanglerPath().string().c_str());
            return;
        }
        const auto name = demangler->Demangle("_$s7SwiftUI8MaterialVAAE5LayerV7opacityyAESdF");
        Expect(name.has_value() && name->find("SwiftUI.Material.Layer.opacity") != std::string::npos &&
                   name->find("Swift.Double") != std::string::npos,
               "the demangled opacity accessor names its type and its Double parameter");
    }
}

int main()
{
    GateParseArguments();
    GateTowers();
    GateDemangler();
    return Finish();
}
