// Sherlock — tools/Sherlock/Source/SherlockCli/Arguments.cpp
// argv walk: flags with a value consume the next token, "--towers" is boolean, an unknown "--x" is a Usage error.
#include <SherlockCli/Arguments.h>

#include <cstdlib>

namespace Sherlock::Cli
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        std::filesystem::path DefaultTowersPath()
        {
            std::filesystem::path dir = std::filesystem::current_path();
            for (int depth = 0; depth < 32; ++depth)
            {
                const auto candidate = dir / "References" / "scripts" / "towers.json";
                if (std::filesystem::exists(candidate))
                {
                    return candidate;
                }
                const auto parent = dir.parent_path();
                if (parent.empty() || parent == dir)
                {
                    break;
                }
                dir = parent;
            }
            return {};
        }
    }

    Expected<Invocation> ParseArguments(int argc, char** argv)
    {
        if (argc < 2)
        {
            return Fail(DiagnosticCode::Usage, Severity::NotVerified, "ParseArguments", "argv", "no command given",
                        "pass a command: build, q, callers, calls, refs, status, --version");
        }
        std::vector<std::string> args(argv + 1, argv + argc);

        Invocation invocation;
        if (args[0] == "--version")
        {
            invocation.Command = "version";
            return invocation;
        }
        invocation.Command = args[0];

        if (const char* env = std::getenv("SHERLOCK_CACHE"); env != nullptr)
        {
            invocation.Cache = env;
        }
        if (const char* env = std::getenv("SHERLOCK_STORE"); env != nullptr)
        {
            invocation.Store = env;
        }
        invocation.Towers = DefaultTowersPath();

        for (std::size_t i = 1; i < args.size(); ++i)
        {
            const std::string& arg = args[i];
            const auto         next = [&]() -> Expected<std::string> {
                if (i + 1 >= args.size())
                {
                    return Fail(DiagnosticCode::Usage, Severity::NotVerified, "ParseArguments", arg,
                                "missing value for this flag", "pass a value after it");
                }
                return args[++i];
            };

            if (arg == "--cache")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                invocation.Cache = *v;
            }
            else if (arg == "--store")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                invocation.Store = *v;
            }
            else if (arg == "--towers")
            {
                invocation.AllTowers = true;
            }
            else if (arg == "--images")
            {
                while (i + 1 < args.size() && !args[i + 1].starts_with("--"))
                {
                    invocation.Images.push_back(args[++i]);
                }
            }
            else if (arg == "--resume")
            {
                invocation.Resume = true;
            }
            else if (arg == "--full")
            {
                invocation.Full = true;
            }
            else if (arg == "--json")
            {
                invocation.Json = true;
            }
            else if (arg == "--workers")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                invocation.Workers = static_cast<unsigned>(std::stoul(*v));
            }
            else if (arg == "--to")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                invocation.To = std::stoull(*v, nullptr, 16);
            }
            else if (arg.starts_with("--"))
            {
                return Fail(DiagnosticCode::Usage, Severity::NotVerified, "ParseArguments", arg, "unknown flag",
                            "check Sherlock --help");
            }
            else
            {
                invocation.Positional.push_back(arg);
            }
        }
        return invocation;
    }
}
