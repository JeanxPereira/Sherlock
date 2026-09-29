// Sherlock — Source/SherlockCli/Arguments.cpp
// argv walk: flags with a value consume the next token, "--towers" is boolean, an unknown "--x" is a Usage error.
#include <SherlockCli/Arguments.h>

#include <cstdlib>
#include <charconv>
#include <limits>

namespace Sherlock::Cli
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        template <class T>
        Expected<T> ParseUnsigned(std::string_view text, int base, std::string_view flag)
        {
            if (text.empty() || text.front() == '-')
            {
                return Fail(DiagnosticCode::Usage, Severity::NotVerified, "ParseArguments", std::string(flag),
                            "the value is not an unsigned integer", "pass a complete numeric value");
            }
            if (base == 16 && text.starts_with("0x"))
            {
                text.remove_prefix(2);
            }
            T value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                return Fail(DiagnosticCode::Usage, Severity::NotVerified, "ParseArguments", std::string(flag),
                            "the value is not a complete integer", "pass a complete numeric value");
            }
            return value;
        }

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

        std::filesystem::path DefaultRepoRoot()
        {
            std::filesystem::path dir = std::filesystem::current_path();
            for (int depth = 0; depth < 32; ++depth)
            {
                if (std::filesystem::is_directory(dir / "docs" / "re") && std::filesystem::is_directory(dir / "Source"))
                {
                    return dir;
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
                        "pass a command: build, q, callers, calls, refs, fn, grep, status, --version");
        }
        std::vector<std::string> args(argv + 1, argv + argc);

        Invocation invocation;
        bool documentsExplicit = false;
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
        invocation.Repo = DefaultRepoRoot();
        if (const char* env = std::getenv("SHERLOCK_REPO"); env != nullptr)
        {
            invocation.Repo = env;
        }
        if (const char* env = std::getenv("SHERLOCK_DOCUMENTS"); env != nullptr)
        {
            invocation.Documents = env;
            documentsExplicit = true;
        }
        if (const char* env = std::getenv("SHERLOCK_IMAGES"); env != nullptr)
        {
            invocation.ImagesDir = env;
        }
        if (const char* env = std::getenv("SHERLOCK_IDA_DIR"); env != nullptr)
        {
            invocation.IdaDir = env;
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
            else if (arg == "--repo")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                invocation.Repo = *v;
            }
            else if (arg == "--documents")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                invocation.Documents = *v;
                documentsExplicit = true;
            }
            else if (arg == "--min-free-memory")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                const auto bytes = ParseUnsigned<std::uint64_t>(*v, 10, arg);
                if (!bytes) return std::unexpected(bytes.error());
                invocation.MinimumFreeMemoryBytes = *bytes;
            }
            else if (arg == "--large-image-bytes")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                const auto bytes = ParseUnsigned<std::uint64_t>(*v, 10, arg);
                if (!bytes) return std::unexpected(bytes.error());
                invocation.LargeImageBytes = *bytes;
            }
            else if (arg == "--images-dir")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                invocation.ImagesDir = *v;
            }
            else if (arg == "--ida-dir")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                invocation.IdaDir = *v;
            }
            else if (arg == "--towers")
            {
                invocation.AllTowers = true;
            }
            else if (arg == "--images")
            {
                // Space-separated AND comma-separated, because both are what a
                // reader writes and a name with a comma in it is not an image.
                while (i + 1 < args.size() && !args[i + 1].starts_with("--"))
                {
                    const std::string& list = args[++i];
                    for (std::size_t at = 0; at <= list.size();)
                    {
                        const auto comma = list.find(',', at);
                        const auto end   = comma == std::string::npos ? list.size() : comma;
                        if (end > at) invocation.Images.push_back(list.substr(at, end - at));
                        if (comma == std::string::npos) break;
                        at = comma + 1;
                    }
                }
            }
            else if (arg == "--resume")
            {
                invocation.Resume = true;
            }
            else if (arg == "--asm")
            {
                invocation.Asm = true;
            }
            else if (arg == "--ignore-case")
            {
                invocation.IgnoreCase = true;
            }
            else if (arg == "--limit")
            {
                if (i + 1 >= args.size())
                {
                    return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "ParseArguments", arg,
                                "--limit takes a count", "pass --limit <hits>");
                }
                invocation.Limit = std::strtoull(args[++i].c_str(), nullptr, 10);
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
                const auto workers = ParseUnsigned<unsigned>(*v, 10, arg);
                if (!workers || *workers == 0 || *workers > 256)
                {
                    return workers ? Fail(DiagnosticCode::Usage, Severity::NotVerified, "ParseArguments", arg,
                                          "worker count is outside 1..256", "pass a worker count from 1 through 256")
                                   : std::unexpected(workers.error());
                }
                invocation.Workers = *workers;
            }
            else if (arg == "--to")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                const auto to = ParseUnsigned<std::uint64_t>(*v, 16, arg);
                if (!to) return std::unexpected(to.error());
                invocation.To = *to;
            }
            else if (arg == "--min-free-bytes")
            {
                const auto v = next();
                if (!v) return std::unexpected(v.error());
                const auto bytes = ParseUnsigned<std::uint64_t>(*v, 10, arg);
                if (!bytes) return std::unexpected(bytes.error());
                invocation.MinimumFreeBytes = *bytes;
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
        if (!documentsExplicit && !invocation.Repo.empty())
        {
            invocation.Documents = invocation.Repo / "build" / "Sherlock" / "Documents.db";
        }
        return invocation;
    }
}
