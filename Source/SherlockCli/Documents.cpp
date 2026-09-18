// Sherlock — tools/Sherlock/Source/SherlockCli/Documents.cpp
// find and laudo query Documents.db and verify a returned document's one backing file.
#include <SherlockCli/Documents.h>

#include <DocumentIndex/FileStamp.h>
#include <SherlockCli/Queries.h>
#include <Store/Database.h>
#include <Store/Schema.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <fcntl.h>
#include <io.h>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Sherlock::Cli
{
    namespace
    {
        using Coverage = std::pair<std::uint64_t, std::uint64_t>;

        Foundation::Expected<Store::Database> OpenDocuments(const QueryEnvironment& env)
        {
            std::error_code error;
            if (env.Documents.empty())
            {
                return Foundation::Fail(Foundation::DiagnosticCode::NotFound, Foundation::Severity::NotVerified,
                                        "OpenDocuments", env.Documents.string(), "Documents.db has not been built",
                                        "run Sherlock build docs");
            }
            const bool exists = std::filesystem::exists(env.Documents, error);
            if (error)
            {
                return Foundation::Fail(Foundation::DiagnosticCode::Io, Foundation::Severity::NotVerified,
                                        "OpenDocuments", env.Documents.string(), "the Documents.db path cannot be read",
                                        "check the path and its permissions", error.message());
            }
            if (!exists)
            {
                return Foundation::Fail(Foundation::DiagnosticCode::NotFound, Foundation::Severity::NotVerified,
                                        "OpenDocuments", env.Documents.string(), "Documents.db has not been built",
                                        "run Sherlock build docs");
            }
            auto db = Store::Database::Open(env.Documents, Store::Database::Mode::ReadOnly);
            if (!db) return std::unexpected(db.error());
            if (auto ok = Store::CheckDocumentsSchema(*db); !ok) return std::unexpected(ok.error());
            return db;
        }

        Foundation::Expected<Coverage> DocumentsCoverage(Store::Database& db)
        {
            auto statement = db.Prepare("SELECT COALESCE(SUM(Read), 0), COALESCE(SUM(Total), 0) FROM Coverage");
            if (!statement) return std::unexpected(statement.error());
            const auto row = statement->Step();
            if (!row) return std::unexpected(row.error());
            if (!*row) return Coverage{};
            return Coverage{static_cast<std::uint64_t>(statement->Int(0)), static_cast<std::uint64_t>(statement->Int(1))};
        }

        Foundation::Expected<bool> IsStale(Store::Database& db, const std::filesystem::path& repo,
                                           std::string_view relativeFile)
        {
            auto statement = db.Prepare("SELECT Size, MTime FROM File WHERE Path = ?1");
            if (!statement) return std::unexpected(statement.error());
            if (auto ok = statement->Bind(1, relativeFile); !ok) return std::unexpected(ok.error());
            const auto row = statement->Step();
            if (!row) return std::unexpected(row.error());
            if (!*row)
            {
                // Every Section a build writes has a matching File row for its own source file
                // (Builder.cpp writes File before Section, for every indexed document) -- a
                // Section with none is Documents.db missing its own bookkeeping, not a fresh
                // file. Reporting "false" (fresh) here fed laudo's byte-exact promise from a
                // "could not look" (finding 4); this is the same fact, refused instead of guessed.
                return Foundation::Fail(Foundation::DiagnosticCode::Mismatch, Foundation::Severity::NotVerified,
                                        "IsStale", std::string(relativeFile),
                                        "this section's own File row is missing from Documents.db",
                                        "rebuild it with Sherlock build docs");
            }
            const auto current = DocumentIndex::StatFile(repo / std::string(relativeFile));
            return !current || current->Size != static_cast<std::uintmax_t>(statement->Int(0)) ||
                   current->MTime != statement->Int(1);
        }

        std::string Basename(std::string_view path)
        {
            const auto slash = path.rfind('/');
            return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
        }

        void EmitLine(const QueryEnvironment& env, std::string line)
        {
            if (env.Output != nullptr) env.Output->push_back(line);
            if (!env.Json) std::printf("%s\n", line.c_str());
        }

        Foundation::Expected<void> EmitText(const QueryEnvironment& env, std::string_view text)
        {
            if (env.Json)
            {
                if (env.Output != nullptr) env.Output->emplace_back(text);
                return {};
            }
            if (env.PlainTextSink)
            {
                if (auto ok = env.PlainTextSink(text); !ok) return std::unexpected(ok.error());
            }
            else
            {
                if (_setmode(_fileno(stdout), _O_BINARY) == -1)
                {
                    return Foundation::Fail(Foundation::DiagnosticCode::Io, Foundation::Severity::NotVerified,
                                            "EmitText", "stdout", "stdout cannot switch to binary mode", "retry the command");
                }
                std::size_t written = 0;
                while (written < text.size())
                {
                    const auto count = std::fwrite(text.data() + written, 1, text.size() - written, stdout);
                    if (count == 0)
                    {
                        return Foundation::Fail(Foundation::DiagnosticCode::Io, Foundation::Severity::NotVerified,
                                                "EmitText", "stdout", "stdout cannot write the laudo text", "retry the command");
                    }
                    written += count;
                }
                if (std::fflush(stdout) != 0 || std::ferror(stdout))
                {
                    return Foundation::Fail(Foundation::DiagnosticCode::Io, Foundation::Severity::NotVerified,
                                            "EmitText", "stdout", "stdout cannot flush the laudo text", "retry the command");
                }
            }
            if (env.Output != nullptr) env.Output->emplace_back(text);
            return {};
        }

        bool LooksNumeric(std::string_view text)
        {
            if (text.empty()) return false;
            return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; });
        }

        // A string under 3 significant characters cannot produce a single trigram, so
        // SectionFtsText's MATCH is guaranteed to answer EMPTY, whatever the corpus holds --
        // that is the instrument's floor, not a fact about the corpus, and find must refuse it
        // as NOT VERIFIED rather than report a silent, always-true "not in the corpus" (finding 6).
        bool BelowTrigramFloor(std::string_view text)
        {
            std::size_t significant = 0;
            for (const unsigned char c : text)
            {
                if (c == '"' || std::isspace(c)) continue;
                ++significant;
                if (significant >= 3) return false;
            }
            return true;
        }

        struct SectionHit
        {
            std::string Text;
            std::string File;
        };

        // laudo matches a slug by path suffix across every indexed root, so a basename present
        // under both docs/re and docs/concepts would otherwise splice two documents into one
        // outline and stat only the first for staleness. No such collision exists in the corpus
        // today, but a query that could silently pick either file is a hole, not a feature --
        // this makes the ambiguity a refusal instead of an arbitrary pick (finding 8).
        Foundation::Expected<void> CheckSlugUnambiguous(Store::Database& db, std::string_view suffix)
        {
            auto statement = db.Prepare("SELECT DISTINCT File FROM Section WHERE substr(File, -length(?1)) = ?1 "
                                        "ORDER BY File LIMIT 2");
            if (!statement) return std::unexpected(statement.error());
            if (auto ok = statement->Bind(1, suffix); !ok) return std::unexpected(ok.error());
            std::vector<std::string> files;
            for (;;)
            {
                const auto row = statement->Step();
                if (!row) return std::unexpected(row.error());
                if (!*row) break;
                files.emplace_back(statement->Text(0));
            }
            if (files.size() > 1)
            {
                return Foundation::Fail(Foundation::DiagnosticCode::Mismatch, Foundation::Severity::NotVerified,
                                        "RunLaudo", std::string(suffix),
                                        std::format("this slug matches more than one file, at least {} and {}",
                                                    files[0], files[1]),
                                        "pass more of the path to disambiguate, or rename one of the two documents");
            }
            return {};
        }

        Foundation::Expected<std::optional<SectionHit>> FindSection(Store::Database& db, std::string_view suffix,
                                                                      std::string_view section, bool numeric,
                                                                      bool insensitive = false)
        {
            const std::string sql = numeric
                ? "SELECT Text, File FROM Section WHERE substr(File, -length(?1)) = ?1 AND Number = ?2 LIMIT 1"
                : insensitive
                    ? "SELECT Text, File FROM Section WHERE substr(File, -length(?1)) = ?1 "
                      "AND instr(lower(Title), lower(?2)) > 0 LIMIT 1"
                    : "SELECT Text, File FROM Section WHERE substr(File, -length(?1)) = ?1 "
                      "AND instr(Title, ?2) > 0 LIMIT 1";
            auto statement = db.Prepare(sql);
            if (!statement) return std::unexpected(statement.error());
            if (auto ok = statement->Bind(1, suffix); !ok) return std::unexpected(ok.error());
            if (auto ok = statement->Bind(2, section); !ok) return std::unexpected(ok.error());
            const auto row = statement->Step();
            if (!row) return std::unexpected(row.error());
            if (!*row) return std::optional<SectionHit>{};
            return std::optional<SectionHit>{SectionHit{std::string(statement->Text(0)), std::string(statement->Text(1))}};
        }
    }

    Verdict RunFind(const QueryEnvironment& env, std::string_view text)
    {
        auto db = OpenDocuments(env);
        if (!db) return {VerdictKind::NotVerified, 0, db.error().Format(), 0, 0};
        const auto coverage = DocumentsCoverage(*db);
        if (!coverage) return {VerdictKind::NotVerified, 0, coverage.error().Format(), 0, 0};
        if (BelowTrigramFloor(text))
        {
            return {VerdictKind::NotVerified, 0,
                    std::format("'{}' has fewer than 3 significant characters -- the trigram index cannot answer it",
                                text),
                    coverage->first, coverage->second};
        }
        auto statement = db->Prepare(
            "SELECT Section.File, Section.Number, Section.Title, "
            "snippet(SectionFtsText, 0, '>>>', '<<<', ' ... ', 24) "
            "FROM SectionFtsText JOIN Section ON Section.Id = SectionFtsText.rowid "
            "WHERE SectionFtsText MATCH ?1 ORDER BY rank");
        if (!statement) return {VerdictKind::NotVerified, 0, statement.error().Format(), coverage->first, coverage->second};
        if (auto ok = statement->Bind(1, text); !ok)
            return {VerdictKind::NotVerified, 0, ok.error().Format(), coverage->first, coverage->second};

        std::size_t count = 0;
        std::map<std::string, bool, std::less<>> staleness;
        for (;;)
        {
            const auto row = statement->Step();
            if (!row) return {VerdictKind::NotVerified, count, row.error().Format(), coverage->first, coverage->second};
            if (!*row) break;
            ++count;
            if (!env.Full && count > 10) continue;
            const std::string file(statement->Text(0));
            auto [where, inserted] = staleness.try_emplace(file, false);
            if (inserted)
            {
                const auto stale = IsStale(*db, env.Repo, file);
                if (!stale)
                    return {VerdictKind::NotVerified, count, stale.error().Format(), coverage->first, coverage->second};
                where->second = *stale;
            }
            const auto number = statement->IsNull(1) ? std::string_view("—") : statement->Text(1);
            EmitLine(env, std::format("  {} {} -- {}{}", Basename(statement->Text(0)), number, statement->Text(2),
                                      where->second ? "  [stale]" : ""));
            EmitLine(env, std::format("    {}", statement->Text(3)));
        }
        return {count == 0 ? VerdictKind::Empty : VerdictKind::Found, count, {}, coverage->first, coverage->second};
    }

    Verdict RunLaudo(const QueryEnvironment& env, std::string_view slug, std::string_view section)
    {
        auto db = OpenDocuments(env);
        if (!db) return {VerdictKind::NotVerified, 0, db.error().Format(), 0, 0};
        const auto coverage = DocumentsCoverage(*db);
        if (!coverage) return {VerdictKind::NotVerified, 0, coverage.error().Format(), 0, 0};
        const std::string suffix = "/" + std::string(slug) + ".md";

        if (auto unambiguous = CheckSlugUnambiguous(*db, suffix); !unambiguous)
        {
            return {VerdictKind::NotVerified, 0, unambiguous.error().Format(), coverage->first, coverage->second};
        }

        if (section.empty())
        {
            auto statement = db->Prepare("SELECT Number, Title, FirstLine, LastLine, File FROM Section "
                                         "WHERE substr(File, -length(?1)) = ?1 ORDER BY FirstLine");
            if (!statement) return {VerdictKind::NotVerified, 0, statement.error().Format(), coverage->first, coverage->second};
            if (auto ok = statement->Bind(1, suffix); !ok)
                return {VerdictKind::NotVerified, 0, ok.error().Format(), coverage->first, coverage->second};
            struct OutlineRow { std::string Number, Title, File; std::int64_t First = 0, Last = 0; };
            std::vector<OutlineRow> rows;
            for (;;)
            {
                const auto row = statement->Step();
                if (!row) return {VerdictKind::NotVerified, rows.size(), row.error().Format(), coverage->first, coverage->second};
                if (!*row) break;
                rows.push_back({statement->IsNull(0) ? "—" : std::string(statement->Text(0)), std::string(statement->Text(1)),
                                std::string(statement->Text(4)), statement->Int(2), statement->Int(3)});
            }
            if (!rows.empty())
            {
                const auto stale = IsStale(*db, env.Repo, rows.front().File);
                if (!stale) return {VerdictKind::NotVerified, 0, stale.error().Format(), coverage->first, coverage->second};
                if (*stale) EmitLine(env, std::format("  stale: {} has changed since Documents.db was built -- this outline may not match", rows.front().File));
            }
            for (const auto& row : rows)
                EmitLine(env, std::format("  {}  {}  ({} line(s))", row.Number, row.Title, row.Last - row.First + 1));
            return {rows.empty() ? VerdictKind::Empty : VerdictKind::Found, rows.size(), {}, coverage->first, coverage->second};
        }

        std::string_view number = section;
        if (number.starts_with("\xC2\xA7")) number.remove_prefix(2);
        else if (!number.empty() && number.front() == static_cast<char>(0xA7)) number.remove_prefix(1);
        const bool numeric = LooksNumeric(number);
        const auto first = FindSection(*db, suffix, number, numeric);
        if (!first) return {VerdictKind::NotVerified, 0, first.error().Format(), coverage->first, coverage->second};
        auto hit = *first;
        if (!hit && !numeric)
        {
            const auto fallback = FindSection(*db, suffix, number, false, true);
            if (!fallback) return {VerdictKind::NotVerified, 0, fallback.error().Format(), coverage->first, coverage->second};
            hit = *fallback;
        }
        if (!hit) return {VerdictKind::Empty, 0, {}, coverage->first, coverage->second};
        const auto stale = IsStale(*db, env.Repo, hit->File);
        if (!stale) return {VerdictKind::NotVerified, 0, stale.error().Format(), coverage->first, coverage->second};
        if (*stale)
            return {VerdictKind::NotVerified, 0,
                    hit->File + " has changed since Documents.db was built -- run Sherlock build docs", coverage->first, coverage->second};
        if (auto emitted = EmitText(env, hit->Text); !emitted)
        {
            return {VerdictKind::NotVerified, 0, emitted.error().Format(), coverage->first, coverage->second};
        }
        return {VerdictKind::Found, 1, {}, coverage->first, coverage->second};
    }
}
