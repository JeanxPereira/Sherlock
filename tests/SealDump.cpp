// Sherlock — tests/Sherlock/SealDump.cpp
// Walks a directory with Sherlock::DocumentIndex::ExtractSeals and prints one TSV row per seal,
// for DocumentIndexParity.py to compare against lint_seals.py -- and against Builder.cpp's own
// WalkSource, which shares DocumentIndex::HasSealExtension with this dump so the two can never
// widen independently again (a shader seal Builder skipped, DocumentIndexParity.py still passed).
#include <DocumentIndex/Builder.h>
#include <DocumentIndex/SealExtractor.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace Sherlock;

namespace
{
    // Matches lint_seals.py's own os.walk exclusion (dirs not in {build, lab, .git}).
    bool UnderExcludedDirectory(const std::filesystem::path& relative)
    {
        for (const auto& part : relative)
        {
            const auto name = part.string();
            if (name == "build" || name == "lab" || name == ".git")
            {
                return true;
            }
        }
        return false;
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: SealDump <repo-root> <source-dir>\n");
        return 2;
    }
    const std::filesystem::path repoRoot(argv[1]);
    const std::filesystem::path sourceDir(argv[2]);
    if (!std::filesystem::is_directory(sourceDir))
    {
        std::fprintf(stderr, "NOT VERIFIED: %s is not a directory\n", argv[2]);
        return 2;
    }

    int exitCode = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceDir))
    {
        if (!entry.is_regular_file() || !DocumentIndex::HasSealExtension(entry.path()))
        {
            continue;
        }
        const auto relative = std::filesystem::relative(entry.path(), repoRoot);
        if (UnderExcludedDirectory(std::filesystem::relative(entry.path(), sourceDir)))
        {
            continue;
        }
        const auto relativeText = relative.generic_string();
        auto       seals        = DocumentIndex::ExtractSeals(entry.path(), relativeText);
        if (!seals)
        {
            std::fprintf(stderr, "FAIL: %s: %s\n", relativeText.c_str(), seals.error().Format().c_str());
            exitCode = 1;
            continue;
        }
        for (const auto& row : *seals)
        {
            const std::string addressText = row.Address ? std::to_string(*row.Address) : std::string();
            std::printf("%s\t%zu\t%s\t%s\t%s\n", row.File.c_str(), row.Line, row.Tag.c_str(),
                       row.Image.value_or("").c_str(), addressText.c_str());
        }
    }
    return exitCode;
}
