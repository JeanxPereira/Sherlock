// Sherlock — tools/Sherlock/Source/DocumentIndex/GitHead.cpp
// Resolves normal and linked-worktree HEAD files without starting git.
#include <DocumentIndex/GitHead.h>

#include <fstream>
#include <sstream>

namespace Sherlock::DocumentIndex
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        Foundation::Expected<std::string> ReadFileTrimmed(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "ReadCurrentHead", path.string(),
                            "the file cannot be opened", "check the repository's git metadata");
            }
            return ReadGitText(stream, path.string());
        }
    }

    Foundation::Expected<std::string> ReadGitText(std::istream& stream, std::string_view diagnosticSubject)
    {
        std::string text;
        try
        {
            for (;;)
            {
                const auto character = stream.get();
                if (character == std::char_traits<char>::eof()) break;
                text.push_back(static_cast<char>(character));
            }
        }
        catch (const std::ios_base::failure&)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "ReadCurrentHead", std::string(diagnosticSubject),
                        "the file cannot be read", "check the repository's git metadata");
        }
        if (!stream.eof() || stream.bad())
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "ReadCurrentHead", std::string(diagnosticSubject),
                        "the file cannot be read", "check the repository's git metadata");
        }
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            {
                text.pop_back();
            }
            return text;
    }

    namespace
    {
        Foundation::Expected<std::filesystem::path> GitDirectory(const std::filesystem::path& repoRoot)
        {
            const auto dotGit = repoRoot / ".git";
            std::error_code typeError;
            if (std::filesystem::is_directory(dotGit, typeError))
            {
                return dotGit;
            }
            if (typeError)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "ReadCurrentHead", dotGit.string(),
                            "the git metadata cannot be inspected", "check the repository's git metadata");
            }
            auto pointer = ReadFileTrimmed(dotGit);
            if (!pointer)
            {
                return std::unexpected(pointer.error());
            }
            constexpr std::string_view prefix = "gitdir: ";
            if (!pointer->starts_with(prefix))
            {
                return Fail(DiagnosticCode::Malformed, Severity::NotVerified, "ReadCurrentHead", dotGit.string(),
                            "the .git file is not a gitdir pointer", "check the linked worktree metadata");
            }
            auto path = std::filesystem::path(pointer->substr(prefix.size()));
            if (path.is_relative())
            {
                path = dotGit.parent_path() / path;
            }
            return path.lexically_normal();
        }

        Foundation::Expected<std::vector<std::filesystem::path>> RefDirectories(const std::filesystem::path& gitDir)
        {
            std::vector<std::filesystem::path> directories{gitDir};
            const auto commonFile = gitDir / "commondir";
            std::error_code existsError;
            if (!std::filesystem::exists(commonFile, existsError))
            {
                if (existsError)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "ReadCurrentHead", commonFile.string(),
                                "the common git directory cannot be inspected", "check the linked worktree metadata");
                }
                return directories;
            }
            auto common = ReadFileTrimmed(commonFile);
            if (!common) return std::unexpected(common.error());
            auto path = std::filesystem::path(*common);
            if (path.is_relative()) path = gitDir / path;
            directories.push_back(path.lexically_normal());
            return directories;
        }

        Foundation::Expected<std::string> ReadPackedRef(const std::vector<std::filesystem::path>& gitDirs,
                                                         std::string_view ref)
        {
            for (const auto& gitDir : gitDirs)
            {
                const auto path = gitDir / "packed-refs";
                std::error_code existsError;
                if (!std::filesystem::exists(path, existsError))
                {
                    if (existsError)
                    {
                        return Fail(DiagnosticCode::Io, Severity::NotVerified, "ReadCurrentHead", path.string(),
                                    "the packed refs cannot be inspected", "check the repository's git metadata");
                    }
                    continue;
                }
                auto packed = ReadFileTrimmed(path);
                if (!packed) return std::unexpected(packed.error());
                std::istringstream lines(*packed);
                std::string line;
                while (std::getline(lines, line))
                {
                    const auto space = line.find(' ');
                    if (space != std::string::npos && std::string_view(line).substr(space + 1) == ref)
                    {
                        return line.substr(0, space);
                    }
                }
            }
            return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "ReadCurrentHead", std::string(ref),
                        "the ref is neither loose nor packed", "run `git rev-parse HEAD` and compare by hand");
        }
    }

    Foundation::Expected<std::string> ReadCurrentHead(const std::filesystem::path& repoRoot)
    {
        auto gitDir = GitDirectory(repoRoot);
        if (!gitDir)
        {
            return std::unexpected(gitDir.error());
        }
        auto head = ReadFileTrimmed(*gitDir / "HEAD");
        if (!head)
        {
            return std::unexpected(head.error());
        }
        constexpr std::string_view prefix = "ref: ";
        if (!head->starts_with(prefix))
        {
            return *head;
        }
        const auto ref = std::string_view(*head).substr(prefix.size());
        auto refDirectories = RefDirectories(*gitDir);
        if (!refDirectories) return std::unexpected(refDirectories.error());
        for (const auto& directory : *refDirectories)
        {
            const auto loosePath = directory / std::filesystem::path(ref);
            std::error_code existsError;
            if (std::filesystem::exists(loosePath, existsError)) return ReadFileTrimmed(loosePath);
            if (existsError)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "ReadCurrentHead", loosePath.string(),
                            "the loose ref cannot be inspected", "check the repository's git metadata");
            }
        }
        return ReadPackedRef(*refDirectories, ref);
    }
}
