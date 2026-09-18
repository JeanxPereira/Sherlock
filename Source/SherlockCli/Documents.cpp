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
#include <map>
#include <optional>
#include <string>
#include <utility>

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
            if (!*row) return false;
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

        void EmitText(const QueryEnvironment& env, std::string text)
        {
            if (env.Output != nullptr) env.Output->push_back(text);
            if (!env.Json) std::fwrite(text.data(), 1, text.size(), stdout);
        }

        bool LooksNumeric(std::string_view text)
        {
            if (text.empty()) return false;
            return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; });
        }

        struct SectionHit
        {
            std::string Text;
            std::string File;
        };

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
        const bool numeric = LooksNumeric(number);
        const auto first = FindSection(*db, suffix, numeric ? number : section, numeric);
        if (!first) return {VerdictKind::NotVerified, 0, first.error().Format(), coverage->first, coverage->second};
        auto hit = *first;
        if (!hit && !numeric)
        {
            const auto fallback = FindSection(*db, suffix, section, false, true);
            if (!fallback) return {VerdictKind::NotVerified, 0, fallback.error().Format(), coverage->first, coverage->second};
            hit = *fallback;
        }
        if (!hit) return {VerdictKind::Empty, 0, {}, coverage->first, coverage->second};
        const auto stale = IsStale(*db, env.Repo, hit->File);
        if (!stale) return {VerdictKind::NotVerified, 0, stale.error().Format(), coverage->first, coverage->second};
        if (*stale)
            return {VerdictKind::NotVerified, 0,
                    hit->File + " has changed since Documents.db was built -- run Sherlock build docs", coverage->first, coverage->second};
        EmitText(env, std::move(hit->Text));
        return {VerdictKind::Found, 1, {}, coverage->first, coverage->second};
    }
}
