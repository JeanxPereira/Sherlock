// Sherlock — Source/DocumentIndex/CitationExtractor.cpp
// Two independent regex passes over the section's text: bare/backticked addresses, backticked symbols.
#include <DocumentIndex/CitationExtractor.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <set>

namespace Sherlock::DocumentIndex
{
    namespace
    {
        bool HasFileExtension(std::string_view token)
        {
            // Source-and-docs extensions, plus the Apple binary/asset file names a laudo cites
            // by name (a framework, a dylib, a shader, a rootfs bundle, an Info.plist-style file,
            // a font, a compiled asset catalog): every one measured as a real backtick-quoted
            // false positive in docs/re + docs/concepts (never a genuine dotted symbol there).
            static constexpr std::string_view kExtensions[] = {
                ".h",     ".hpp",  ".cpp",   ".md",    ".py",       ".json",     ".cmake",
                ".ps1",   ".txt",  ".framework", ".dylib", ".frag", ".app",      ".plist",
                ".ttf",   ".ttc",  ".car",   ".bundle", ".metallib"};
            for (const auto ext : kExtensions)
            {
                if (token.size() > ext.size() &&
                    std::equal(ext.begin(), ext.end(), token.end() - ext.size(),
                               [](char a, char b) {
                                   return std::tolower(static_cast<unsigned char>(a)) ==
                                          std::tolower(static_cast<unsigned char>(b));
                               }))
                {
                    return true;
                }
            }
            return false;
        }

        bool LooksLikeSymbol(std::string_view token)
        {
            if (token.find('/') != std::string_view::npos || token.find('\\') != std::string_view::npos)
            {
                return false;
            }
            if (HasFileExtension(token))
            {
                return false;
            }
            static const std::regex mangled(R"(^_\$s[A-Za-z0-9_]+$)");
            static const std::regex dotted(R"(^_?\$?[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$)");
            const std::string       owned(token);
            return std::regex_match(owned, mangled) || std::regex_match(owned, dotted);
        }
    }

    std::vector<Citation> ExtractCitations(std::string_view text)
    {
        std::set<std::uint64_t> addresses;
        std::set<std::string>   symbols;

        // The cache-address SHAPE (lint_seals.py's own ADDR), not a bare hex-digit-count test: a
        // struct's `flags=0x00000052` or a float's `0x00000000` bit pattern is six-plus hex digits
        // long, but neither is a cache VA -- both start with zero, which the shape test refuses
        // structurally, the same way a cache image's seal can never wear a rootfs address
        // (SealExtractor.cpp's own cacheAddress).
        static const std::regex addressPattern(R"(0x(1[89a-f][0-9a-f]{7}|2[0-9a-f]{8})\b)");
        for (auto it = std::cregex_iterator(text.data(), text.data() + text.size(), addressPattern);
             it != std::cregex_iterator(); ++it)
        {
            std::uint64_t value  = 0;
            const auto    match  = it->str();
            std::from_chars(match.data() + 2, match.data() + match.size(), value, 16);
            addresses.insert(value);
        }

        static const std::regex backtick(R"(`([^`]+)`)");
        for (auto it = std::cregex_iterator(text.data(), text.data() + text.size(), backtick);
             it != std::cregex_iterator(); ++it)
        {
            const auto token = (*it)[1].str();
            if (LooksLikeSymbol(token))
            {
                symbols.insert(token);
            }
        }

        std::vector<Citation> citations;
        citations.reserve(addresses.size() + symbols.size());
        for (const auto address : addresses)
        {
            citations.push_back({address, std::nullopt});
        }
        for (const auto& symbol : symbols)
        {
            citations.push_back({std::nullopt, symbol});
        }
        return citations;
    }
}
