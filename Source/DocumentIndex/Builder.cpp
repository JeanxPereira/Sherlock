// Sherlock — tools/Sherlock/Source/DocumentIndex/Builder.cpp
// Orchestrates the document extractors into one Documents.db transaction.
#include <DocumentIndex/Builder.h>

#include <DocumentIndex/CitationExtractor.h>
#include <DocumentIndex/ConceptFrontMatter.h>
#include <DocumentIndex/FileStamp.h>
#include <DocumentIndex/GitHead.h>
#include <DocumentIndex/LaudoSections.h>
#include <DocumentIndex/SealExtractor.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include <charconv>
#include <chrono>
#include <system_error>
#include <vector>

namespace Sherlock::DocumentIndex
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    namespace
    {
        Foundation::Expected<std::vector<std::filesystem::path>> WalkMarkdown(const std::filesystem::path& dir)
        {
            std::error_code error;
            if (!std::filesystem::exists(dir, error))
            {
                if (error)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", dir.string(),
                                "the directory cannot be inspected", "check the repository layout");
                }
                return std::vector<std::filesystem::path>{};
            }
            std::vector<std::filesystem::path> files;
            std::filesystem::directory_iterator iterator(dir, error), end;
            if (error)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", dir.string(),
                            "the directory cannot be read", "check the repository layout");
            }
            while (iterator != end)
            {
                if (iterator->is_regular_file(error) && !error && IsIndexedMarkdown(iterator->path()))
                {
                    files.push_back(iterator->path());
                }
                if (error)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", iterator->path().string(),
                                "a directory entry cannot be inspected", "check the repository layout");
                }
                iterator.increment(error);
                if (error)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", dir.string(),
                                "the directory cannot be walked", "check the repository layout");
                }
            }
            return files;
        }

        Foundation::Expected<std::vector<std::filesystem::path>> WalkSource(
            const std::filesystem::path& dir, const std::function<Foundation::Expected<void>()>& afterIncrement = {})
        {
            std::error_code error;
            if (!std::filesystem::exists(dir, error))
            {
                if (error)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", dir.string(),
                                "the Source directory cannot be inspected", "check the repository layout");
                }
                return std::vector<std::filesystem::path>{};
            }
            std::vector<std::filesystem::path> files;
            std::filesystem::recursive_directory_iterator iterator(dir, error), end;
            if (error)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", dir.string(),
                            "the Source directory cannot be read", "check the repository layout");
            }
            while (iterator != end)
            {
                const auto name = iterator->path().filename().string();
                bool skip = false;
                if (iterator->is_directory(error) && !error && (name == "build" || name == "lab" || name == ".git"))
                {
                    iterator.disable_recursion_pending();
                    skip = true;
                }
                if (error)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", iterator->path().string(),
                                "a Source entry cannot be inspected", "check the repository layout");
                }
                const auto extension = iterator->path().extension();
                if (!skip && iterator->is_regular_file(error) && !error &&
                    (extension == ".h" || extension == ".hpp" || extension == ".cpp"))
                {
                    files.push_back(iterator->path());
                }
                if (error)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", iterator->path().string(),
                                "a Source entry cannot be inspected", "check the repository layout");
                }
                iterator.increment(error);
                if (error)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", dir.string(),
                                "the Source directory cannot be walked", "check the repository layout");
                }
                if (afterIncrement)
                {
                    auto injected = afterIncrement();
                    if (!injected) return std::unexpected(injected.error());
                }
            }
            return files;
        }

        Foundation::Expected<void> WriteMeta(Store::Database& db, std::string_view key, std::string_view value)
        {
            auto statement = db.Prepare("INSERT OR REPLACE INTO Meta(Key, Value) VALUES(?1, ?2)");
            if (!statement) return std::unexpected(statement.error());
            if (auto ok = statement->Bind(1, key); !ok) return std::unexpected(ok.error());
            if (auto ok = statement->Bind(2, value); !ok) return std::unexpected(ok.error());
            if (auto ok = statement->Step(); !ok) return std::unexpected(ok.error());
            return {};
        }

        Foundation::Expected<void> WriteCoverage(Store::Database& db, std::string_view root, std::uint64_t read,
                                                 std::uint64_t total)
        {
            auto statement = db.Prepare("INSERT OR REPLACE INTO Coverage(Root, Read, Total) VALUES(?1, ?2, ?3)");
            if (!statement) return std::unexpected(statement.error());
            if (auto ok = statement->Bind(1, root); !ok) return std::unexpected(ok.error());
            if (auto ok = statement->Bind(2, static_cast<std::int64_t>(read)); !ok) return std::unexpected(ok.error());
            if (auto ok = statement->Bind(3, static_cast<std::int64_t>(total)); !ok) return std::unexpected(ok.error());
            if (auto ok = statement->Step(); !ok) return std::unexpected(ok.error());
            return {};
        }

        Foundation::Expected<FileStamp> StatInput(const std::filesystem::path& file)
        {
            const auto stamp = StatFile(file);
            if (!stamp)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", file.string(),
                            "the input cannot be stat'd immediately before reading", "check the file is readable");
            }
            return *stamp;
        }

        Foundation::Expected<void> WriteFile(Store::Statement& statement, std::string_view relative,
                                             const FileStamp& stamp)
        {
            if (auto ok = statement.Reset(); !ok) return std::unexpected(ok.error());
            if (auto ok = statement.Bind(1, relative); !ok) return std::unexpected(ok.error());
            if (auto ok = statement.Bind(2, static_cast<std::int64_t>(stamp.Size)); !ok) return std::unexpected(ok.error());
            if (auto ok = statement.Bind(3, stamp.MTime); !ok) return std::unexpected(ok.error());
            if (auto ok = statement.Step(); !ok) return std::unexpected(ok.error());
            return {};
        }

        Foundation::Expected<void> RemoveDatabaseFile(const std::filesystem::path& path)
        {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (error)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", path.string(),
                            "the database path cannot be inspected", "check the output path");
            }
            if (exists && std::filesystem::is_directory(path, error))
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", path.string(),
                            "a database path is an existing directory", "pass a database file path");
            }
            if (error)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", path.string(),
                            "the database path cannot be inspected", "check the output path");
            }
            std::filesystem::remove(path, error);
            if (error)
            {
                return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", path.string(),
                            "an existing documents database file cannot be removed", "close its users and retry");
            }
            return {};
        }

        std::string RepoRelative(const std::filesystem::path& root, const std::filesystem::path& file)
        {
            return std::filesystem::relative(file, root).generic_string();
        }

        // Removes a temp build's leftovers unconditionally at scope exit -- a no-op once
        // PublishDocuments has moved them to their final path, so it only ever cleans up a build
        // that returns early on a failure (decision: build-to-temp-then-publish, finding 7).
        struct TempDatabaseCleanup
        {
            std::filesystem::path TempPath;
            ~TempDatabaseCleanup()
            {
                std::error_code error;
                for (const std::string_view suffix : {"", "-wal", "-shm"})
                {
                    std::filesystem::remove(std::filesystem::path(TempPath.string() + std::string(suffix)), error);
                }
            }
        };

        // Publishes a fully-built temp store over the final path in one rename per file, only
        // after the temp build commits -- any earlier failure leaves documentsPath exactly as it
        // is (finding 7: a half-built store must never replace a good one).
        Foundation::Expected<void> PublishDocuments(const std::filesystem::path& tempPath,
                                                    const std::filesystem::path& finalPath)
        {
            for (const std::string_view suffix : {"", "-wal", "-shm"})
            {
                const std::filesystem::path from(tempPath.string() + std::string(suffix));
                const std::filesystem::path to(finalPath.string() + std::string(suffix));
                std::error_code existsError;
                const bool fromExists = std::filesystem::exists(from, existsError);
                if (existsError)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", from.string(),
                                "the built documents database cannot be inspected before publishing",
                                "check the output path");
                }
                if (!fromExists)
                {
                    continue;
                }
                if (auto ok = RemoveDatabaseFile(to); !ok)
                {
                    return ok;
                }
                std::error_code renameError;
                std::filesystem::rename(from, to, renameError);
                if (renameError)
                {
                    return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", from.string(),
                                "the built documents database cannot be published to its final path",
                                "check the output path", renameError.message());
                }
            }
            return {};
        }

        void AppendConceptCitations(std::vector<Citation>& citations, std::string_view text)
        {
            const auto frontMatter = ParseFrontMatter(text);
            if (!frontMatter)
            {
                return;
            }
            for (const auto& alias : frontMatter->Aliases)
            {
                if (alias.starts_with("0x"))
                {
                    std::uint64_t value = 0;
                    const auto [end, error] = std::from_chars(alias.data() + 2, alias.data() + alias.size(), value, 16);
                    if (error == std::errc{} && end == alias.data() + alias.size()) citations.push_back({value, std::nullopt});
                }
                else
                {
                    citations.push_back({std::nullopt, alias});
                }
            }
            const auto proseEnd = GeneratedBlockStart(text);
            if (proseEnd > frontMatter->BodyStart)
            {
                auto prose = ExtractCitations(text.substr(frontMatter->BodyStart, proseEnd - frontMatter->BodyStart));
                citations.insert(citations.end(), prose.begin(), prose.end());
            }
        }
    }

    Foundation::Expected<std::vector<std::filesystem::path>> WalkSourceForTesting(
        const std::filesystem::path& root, std::function<Foundation::Expected<void>()> afterIncrement)
    {
        return WalkSource(root, afterIncrement);
    }

    bool IsIndexedMarkdown(const std::filesystem::path& path)
    {
        if (path.extension() != ".md")
        {
            return false;
        }
        const auto name = path.filename().string();
        return name != "README.md" && name != "index.md";
    }

    Foundation::Expected<std::vector<std::filesystem::path>> WalkMarkdownForCli(const std::filesystem::path& dir)
    {
        return WalkMarkdown(dir);
    }

    Foundation::Expected<DocumentsBuildReport> BuildDocuments(const std::filesystem::path& repoRoot,
                                                              const std::filesystem::path& documentsPath)
    {
        const auto start = std::chrono::steady_clock::now();
        const std::filesystem::path tempPath(documentsPath.string() + ".tmp");
        TempDatabaseCleanup cleanup{tempPath};
        for (const auto& path : {tempPath, std::filesystem::path(tempPath.string() + "-wal"),
                                 std::filesystem::path(tempPath.string() + "-shm")})
        {
            if (auto ok = RemoveDatabaseFile(path); !ok) return std::unexpected(ok.error());
        }
        std::error_code directoryError;
        std::filesystem::create_directories(documentsPath.parent_path(), directoryError);
        if (directoryError)
        {
            return Fail(DiagnosticCode::Io, Severity::NotVerified, "BuildDocuments", documentsPath.parent_path().string(),
                        "the documents database directory cannot be created", "check the output path");
        }

        DocumentsBuildReport report;
        // Everything that touches tempPath's connection lives in this block, so the connection
        // (and any WAL/SHM it opened) closes before PublishDocuments renames the file underneath
        // it -- Windows refuses to rename a file a live handle still holds open (finding 7).
        {
        auto database = Store::Database::Open(tempPath, Store::Database::Mode::ReadWrite);
        if (!database) return std::unexpected(database.error());
        auto transaction = Store::Transaction::Begin(*database);
        if (!transaction) return std::unexpected(transaction.error());
        if (auto ok = Store::CreateDocumentsStore(*database); !ok) return std::unexpected(ok.error());

        auto insertSection = database->Prepare("INSERT INTO Section(File, Number, Title, FirstLine, LastLine, Text) VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
        auto insertCitation = database->Prepare("INSERT INTO Citation(Section, Address, Symbol) VALUES(?1, ?2, ?3)");
        auto insertSeal = database->Prepare("INSERT INTO Seal(File, Line, Tag, Image, Symbol, Address) VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
        auto insertFile = database->Prepare("INSERT OR REPLACE INTO File(Path, Size, MTime) VALUES(?1, ?2, ?3)");
        if (!insertSection) return std::unexpected(insertSection.error());
        if (!insertCitation) return std::unexpected(insertCitation.error());
        if (!insertSeal) return std::unexpected(insertSeal.error());
        if (!insertFile) return std::unexpected(insertFile.error());

        const auto indexMarkdown = [&](const std::filesystem::path& root, bool isConcept)
            -> Foundation::Expected<std::pair<std::uint64_t, std::uint64_t>> {
            auto files = WalkMarkdown(root);
            if (!files) return std::unexpected(files.error());
            std::uint64_t read = 0;
            for (const auto& file : *files)
            {
                const auto relative = RepoRelative(repoRoot, file);
                auto stamp = StatInput(file);
                if (!stamp) return std::unexpected(stamp.error());
                auto sections = SplitDocument(file, relative);
                if (!sections) return std::unexpected(sections.error());
                if (auto ok = WriteFile(*insertFile, relative, *stamp); !ok) return std::unexpected(ok.error());
                ++read;
                for (const auto& section : *sections)
                {
                    if (auto ok = insertSection->Reset(); !ok) return std::unexpected(ok.error());
                    if (auto ok = insertSection->Bind(1, section.File); !ok) return std::unexpected(ok.error());
                    if (section.Number) { if (auto ok = insertSection->Bind(2, *section.Number); !ok) return std::unexpected(ok.error()); }
                    else { if (auto ok = insertSection->BindNull(2); !ok) return std::unexpected(ok.error()); }
                    if (auto ok = insertSection->Bind(3, section.Title); !ok) return std::unexpected(ok.error());
                    if (auto ok = insertSection->Bind(4, static_cast<std::int64_t>(section.FirstLine)); !ok) return std::unexpected(ok.error());
                    if (auto ok = insertSection->Bind(5, static_cast<std::int64_t>(section.LastLine)); !ok) return std::unexpected(ok.error());
                    if (auto ok = insertSection->Bind(6, section.Text); !ok) return std::unexpected(ok.error());
                    if (auto ok = insertSection->Step(); !ok) return std::unexpected(ok.error());
                    ++report.SectionsWritten;
                    const auto sectionId = database->LastInsertId();
                    std::vector<Citation> citations;
                    if (isConcept) AppendConceptCitations(citations, section.Text); else citations = ExtractCitations(section.Text);
                    for (const auto& citation : citations)
                    {
                        if (auto ok = insertCitation->Reset(); !ok) return std::unexpected(ok.error());
                        if (auto ok = insertCitation->Bind(1, sectionId); !ok) return std::unexpected(ok.error());
                        if (citation.Address) { if (auto ok = insertCitation->Bind(2, static_cast<std::int64_t>(*citation.Address)); !ok) return std::unexpected(ok.error()); }
                        else { if (auto ok = insertCitation->BindNull(2); !ok) return std::unexpected(ok.error()); }
                        if (citation.Symbol) { if (auto ok = insertCitation->Bind(3, *citation.Symbol); !ok) return std::unexpected(ok.error()); }
                        else { if (auto ok = insertCitation->BindNull(3); !ok) return std::unexpected(ok.error()); }
                        if (auto ok = insertCitation->Step(); !ok) return std::unexpected(ok.error());
                        ++report.CitationsWritten;
                    }
                }
            }
            return std::pair{read, static_cast<std::uint64_t>(files->size())};
        };

        auto laudos = indexMarkdown(repoRoot / "docs" / "re", false);
        if (!laudos) return std::unexpected(laudos.error());
        report.LaudoFilesRead = laudos->first; report.LaudoFilesTotal = laudos->second;
        auto concepts = indexMarkdown(repoRoot / "docs" / "concepts", true);
        if (!concepts) return std::unexpected(concepts.error());
        report.ConceptFilesRead = concepts->first; report.ConceptFilesTotal = concepts->second;
        auto sourceFiles = WalkSource(repoRoot / "Source");
        if (!sourceFiles) return std::unexpected(sourceFiles.error());
        report.SourceFilesTotal = sourceFiles->size();
        for (const auto& file : *sourceFiles)
        {
            const auto relative = RepoRelative(repoRoot, file);
            auto stamp = StatInput(file);
            if (!stamp) return std::unexpected(stamp.error());
            auto seals = ExtractSeals(file, relative);
            if (!seals) return std::unexpected(seals.error());
            if (auto ok = WriteFile(*insertFile, relative, *stamp); !ok) return std::unexpected(ok.error());
            ++report.SourceFilesRead;
            for (const auto& seal : *seals)
            {
                if (auto ok = insertSeal->Reset(); !ok) return std::unexpected(ok.error());
                if (auto ok = insertSeal->Bind(1, seal.File); !ok) return std::unexpected(ok.error());
                if (auto ok = insertSeal->Bind(2, static_cast<std::int64_t>(seal.Line)); !ok) return std::unexpected(ok.error());
                if (auto ok = insertSeal->Bind(3, seal.Tag); !ok) return std::unexpected(ok.error());
                if (seal.Image) { if (auto ok = insertSeal->Bind(4, *seal.Image); !ok) return std::unexpected(ok.error()); }
                else { if (auto ok = insertSeal->BindNull(4); !ok) return std::unexpected(ok.error()); }
                if (auto ok = insertSeal->BindNull(5); !ok) return std::unexpected(ok.error());
                if (seal.Address) { if (auto ok = insertSeal->Bind(6, static_cast<std::int64_t>(*seal.Address)); !ok) return std::unexpected(ok.error()); }
                else { if (auto ok = insertSeal->BindNull(6); !ok) return std::unexpected(ok.error()); }
                if (auto ok = insertSeal->Step(); !ok) return std::unexpected(ok.error());
                ++report.SealsWritten;
            }
        }
        if (auto ok = Store::CreateDocumentIndexes(*database); !ok) return std::unexpected(ok.error());
        if (auto ok = database->Execute("INSERT INTO SectionFtsTitle(rowid, Title) SELECT Id, Title FROM Section"); !ok) return std::unexpected(ok.error());
        if (auto ok = database->Execute("INSERT INTO SectionFtsText(rowid, Text) SELECT Id, Text FROM Section"); !ok) return std::unexpected(ok.error());
        auto head = ReadCurrentHead(repoRoot);
        if (!head) return std::unexpected(head.error());
        report.Head = *head;
        if (auto ok = WriteMeta(*database, "Head", report.Head); !ok) return std::unexpected(ok.error());
        if (auto ok = WriteCoverage(*database, "docs/re", report.LaudoFilesRead, report.LaudoFilesTotal); !ok) return std::unexpected(ok.error());
        if (auto ok = WriteCoverage(*database, "docs/concepts", report.ConceptFilesRead, report.ConceptFilesTotal); !ok) return std::unexpected(ok.error());
        if (auto ok = WriteCoverage(*database, "Source", report.SourceFilesRead, report.SourceFilesTotal); !ok) return std::unexpected(ok.error());
        if (auto ok = transaction->Commit(); !ok) return std::unexpected(ok.error());
        } // database and transaction close here, before the temp file is published

        if (auto ok = PublishDocuments(tempPath, documentsPath); !ok) return std::unexpected(ok.error());
        report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return report;
    }
}
