// Sherlock — tests/Sherlock/DocumentsFixture.cpp
// A deterministic Documents.db fixture, written through the Store schema APIs.
#include <Store/Database.h>
#include <Store/Schema.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

using namespace Sherlock;

namespace
{
    bool InsertFile(Store::Database& db, std::string_view path, const std::filesystem::path& disk)
    {
        auto statement = db.Prepare("INSERT INTO File(Path, Size, MTime) VALUES(?1, ?2, ?3)");
        if (!statement) return false;
        const auto stamp = std::filesystem::last_write_time(disk).time_since_epoch().count();
        return statement->Bind(1, path).has_value() &&
               statement->Bind(2, static_cast<std::int64_t>(std::filesystem::file_size(disk))).has_value() &&
               statement->Bind(3, stamp).has_value() && statement->Step().has_value();
    }

    bool InsertSection(Store::Database& db, std::string_view file, std::string_view number, std::string_view title,
                       std::string_view text)
    {
        auto section = db.Prepare("INSERT INTO Section(File, Number, Title, FirstLine, LastLine, Text) "
                                  "VALUES(?1, ?2, ?3, 1, 3, ?4)");
        if (!section || !section->Bind(1, file) || !section->Bind(2, number) || !section->Bind(3, title) ||
            !section->Bind(4, text) || !section->Step())
        {
            return false;
        }
        auto fts = db.Prepare("INSERT INTO SectionFtsText(rowid, Text) VALUES(?1, ?2)");
        return fts && fts->Bind(1, db.LastInsertId()).has_value() && fts->Bind(2, text).has_value() &&
               fts->Step().has_value();
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: SherlockDocumentsFixture <scratch-root>\n";
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "docs" / "re", error);
    std::filesystem::create_directories(root / "docs" / "concepts", error);
    if (error)
    {
        std::cerr << "FAIL: cannot create fixture directories\n";
        return 1;
    }

    const std::string crlf = "## §7 LayerResolver\r\n\r\n100% shadow pool.\r\n";
    const std::string lf = "A concept page\n";
    const auto laudo = root / "docs" / "re" / "sample.md";
    const auto concept = root / "docs" / "concepts" / "0x27c198c20.md";
    std::ofstream(laudo, std::ios::binary) << crlf;
    std::ofstream(concept, std::ios::binary) << lf;

    auto db = Store::Database::Open(root / "Documents.db", Store::Database::Mode::ReadWrite);
    if (!db || !Store::CreateDocumentsStore(*db) || !Store::CreateDocumentIndexes(*db) ||
        !db->Execute("INSERT INTO Coverage(Root, Read, Total) VALUES('docs/re', 2, 2)") ||
        !InsertFile(*db, "docs/re/sample.md", laudo) || !InsertFile(*db, "docs/concepts/0x27c198c20.md", concept) ||
        !InsertSection(*db, "docs/re/sample.md", "7", "LayerResolver", crlf) ||
        !InsertSection(*db, "docs/concepts/0x27c198c20.md", "", "Concept Title", lf))
    {
        std::cerr << "FAIL: cannot build the Documents.db fixture\n";
        return 1;
    }
    return 0;
}
