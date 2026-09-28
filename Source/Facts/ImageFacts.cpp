// Sherlock — tools/Sherlock/Source/Facts/ImageFacts.cpp
// __TEXT.__text scanned once: calls resolved through BranchIsland, literals through LiteralReaders.
#include <Facts/BranchIsland.h>
#include <Facts/ImageFacts.h>
#include <Facts/LiteralReaders.h>
#include <Facts/VirtualCall.h>

#include <Store/Schema.h>

#include <algorithm>
#include <map>
#include <unordered_map>

namespace Sherlock::Facts
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Hex;
    using Foundation::Severity;

    namespace
    {
        std::uint64_t CallerOf(const std::vector<FunctionRow>& functions, std::uint64_t site)
        {
            const auto it = std::upper_bound(functions.begin(), functions.end(), site,
                                             [](std::uint64_t value, const FunctionRow& f) { return value < f.Address; });
            return it == functions.begin() ? 0 : (it - 1)->Address;
        }
    }

    Expected<ImageFacts> ExtractImage(const DyldSharedCache::Cache& cache, const DyldSharedCache::CacheImage& cacheImage,
                                      Disassembler& disassembler)
    {
        const auto parsed = MachO::Image::Parse(cache, cacheImage.Header);
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }
        const auto* text = parsed->FindSection("__TEXT", "__text");
        if (text == nullptr)
        {
            return Fail(DiagnosticCode::NotFound, Severity::NotVerified, "ExtractImage", cacheImage.Path,
                        "the image carries no __TEXT.__text", "extract facts only from executable images");
        }

        ImageFacts facts;
        facts.Path     = cacheImage.Path;
        facts.Segments = parsed->Segments();
        facts.Sections = parsed->Sections();

        const auto symbols = parsed->Symbols(cache);
        if (!symbols) return std::unexpected(symbols.error());
        facts.Symbols = *symbols;

        const auto rawStarts = parsed->FunctionStarts(cache);
        if (!rawStarts) return std::unexpected(rawStarts.error());
        std::vector<std::uint64_t> starts = *rawStarts;
        std::sort(starts.begin(), starts.end());
        for (std::size_t i = 0; i < starts.size(); ++i)
        {
            const std::uint64_t next = i + 1 < starts.size() ? starts[i + 1] : text->Address + text->Size;
            facts.Functions.push_back({starts[i], next - starts[i]});
        }

        const auto bytes = cache.Read(text->Address, text->Size);
        if (!bytes)
        {
            return std::unexpected(bytes.error());
        }

        LiteralTracker                      literals;
        std::unordered_map<std::uint64_t, std::optional<std::uint64_t>> islandCache;
        std::optional<Foundation::Diagnostic>                            islandError;
        // Keyed by (slot, D): every blraa/braa site sharing a (slot, D) pair reaches the same
        // family, and QuartzCore-sized images repeat a handful of pairs across many sites.
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<VirtualCallCandidate>> vcallFamilyCache;
        std::optional<Foundation::Diagnostic>                                                 vcallError;

        const auto coverage = disassembler.Stream(*bytes, text->Address, [&](const Instruction& ins) {
            const std::uint64_t caller = CallerOf(facts.Functions, ins.Address);

            std::vector<LiteralRead> reads;
            literals.Feed(ins, reads);
            for (const auto& read : reads)
            {
                LiteralRow row;
                row.Site   = read.Site;
                row.Target = read.Target;
                row.Caller = caller;
                row.Kind   = read.Kind;
                if ((read.Kind == "ldr" || read.Kind == "ldur") && !read.Destination.empty() &&
                    read.Destination.front() == 'd')
                {
                    const auto value = cache.Read(read.Target, 8);
                    if (value)
                    {
                        double v = 0.0;
                        std::memcpy(&v, value->data(), sizeof(v));
                        row.Value = v;
                    }
                }
                facts.Literals.push_back(std::move(row));
            }

            if (ins.Mnemonic == "blraa" || ins.Mnemonic == "braa")
            {
                // A bounded lookback, not the whole function: vcall.py's own bulk scan
                // (--callers) uses the same 0x100-byte window, since the mov/movk/add/ldr chain
                // that sets up a PAC vtable dispatch sits immediately upstream of it.
                const std::uint64_t lookback = (ins.Address - text->Address >= 0x100) ? ins.Address - 0x100
                                                                                       : text->Address;
                const std::uint64_t windowStart = std::max(caller, lookback);
                const auto           pattern    = ExtractVirtualCallPattern(cache, disassembler, windowStart, ins.Address);
                if (!pattern)
                {
                    vcallError = pattern.error();
                    return;
                }
                if (*pattern)
                {
                    const auto key     = std::make_pair((*pattern)->SlotOffset, (*pattern)->Discriminator);
                    auto        cached = vcallFamilyCache.find(key);
                    if (cached == vcallFamilyCache.end())
                    {
                        auto family = ResolveVirtualCallFamily(cache, facts.Symbols, key.first, key.second);
                        if (!family)
                        {
                            vcallError = family.error();
                            return;
                        }
                        cached = vcallFamilyCache.emplace(key, std::move(*family)).first;
                    }
                    for (const auto& candidate : cached->second)
                    {
                        facts.VirtualCalls.push_back({ins.Address, key.first, key.second, candidate.Address,
                                                      (*pattern)->Instruction, candidate.Symbol});
                    }
                }
                return;
            }

            if (ins.Mnemonic != "bl")
            {
                return;
            }
            const std::uint64_t target = std::stoull(std::string(ins.Operands.substr(ins.Operands.find('#') + 1)),
                                                      nullptr, 16);
            CallRow row;
            row.Site   = ins.Address;
            row.Caller = caller;
            if (target >= text->Address && target < text->Address + text->Size)
            {
                row.Target = target;
                row.Via    = "Direct";
                facts.Calls.push_back(std::move(row));
                return;
            }
            if (islandCache.find(target) == islandCache.end())
            {
                auto resolved = ResolveIsland(cache, disassembler, target);
                if (!resolved)
                {
                    islandError = resolved.error();
                    islandCache[target] = std::nullopt;
                }
                else
                {
                    islandCache[target] = *resolved;
                }
            }
            const auto& resolved = islandCache[target];
            if (resolved)
            {
                row.Target = *resolved;
                row.Island = target;
                row.Via    = "Island";
            }
            else
            {
                row.Target = target;
                row.Island = target;
                row.Via    = "Unresolved";
            }
            facts.Calls.push_back(std::move(row));
        });

        if (!coverage)
        {
            return std::unexpected(coverage.error());
        }
        facts.Coverage = *coverage;

        if (islandError)
        {
            return std::unexpected(*islandError);
        }
        if (vcallError)
        {
            return std::unexpected(*vcallError);
        }
        return facts;
    }

    namespace
    {
        Expected<std::int64_t> InternName(Store::Database& db, Store::Statement& lookup, Store::Statement& insert,
                                          std::unordered_map<std::string, std::int64_t>& cache, std::string_view text)
        {
            const std::string key(text);
            if (const auto it = cache.find(key); it != cache.end())
            {
                return it->second;
            }
            if (auto ok = lookup.Reset(); !ok)
            {
                return std::unexpected(ok.error());
            }
            if (auto ok = lookup.Bind(1, text); !ok)
            {
                return std::unexpected(ok.error());
            }
            const auto row = lookup.Step();
            if (!row)
            {
                return std::unexpected(row.error());
            }
            if (*row)
            {
                const std::int64_t id = lookup.Int(0);
                cache.emplace(key, id);
                return id;
            }
            if (auto ok = insert.Reset(); !ok)
            {
                return std::unexpected(ok.error());
            }
            if (auto ok = insert.Bind(1, text); !ok)
            {
                return std::unexpected(ok.error());
            }
            if (auto ok = insert.Step(); !ok)
            {
                return std::unexpected(ok.error());
            }
            const std::int64_t id = db.LastInsertId();
            cache.emplace(key, id);
            return id;
        }
    }

    Expected<void> WriteImageFacts(Store::Database& db, const ImageFacts& facts,
                                   const std::function<std::optional<std::string>(std::string_view)>& demangle)
    {
        auto tx = Store::Transaction::Begin(db);
        if (!tx)
        {
            return std::unexpected(tx.error());
        }

        auto nameLookup = db.Prepare("SELECT Id FROM Name WHERE Text = ?1");
        auto nameInsert = db.Prepare("INSERT INTO Name(Text) VALUES(?1)");
        if (!nameLookup || !nameInsert)
        {
            return std::unexpected((!nameLookup ? nameLookup : nameInsert).error());
        }
        std::unordered_map<std::string, std::int64_t> names;

        auto segmentInsert = db.Prepare("INSERT INTO Segment(Name, Address, Size) VALUES(?1, ?2, ?3)");
        auto sectionInsert = db.Prepare("INSERT INTO Section(Name, Address, Size) VALUES(?1, ?2, ?3)");
        auto functionInsert = db.Prepare("INSERT INTO Function(Address, Size, Source) VALUES(?1, ?2, 'FunctionStarts')");
        // The symbol table can carry the exact same (Address, Name) more than once --
        // SystemBannerUI repeats _OUTLINED_FUNCTION_0/1/2 at their own address 2-3 times each,
        // measured directly off LC_SYMTAB. The second occurrence names nothing new over the
        // first, so OR IGNORE keeps the row Symbol's own primary key already promises.
        auto symbolInsert =
            db.Prepare("INSERT OR IGNORE INTO Symbol(Address, Name, Demangled, External) VALUES(?1, ?2, ?3, ?4)");
        auto callInsert = db.Prepare("INSERT INTO Call(Site, Caller, Target, Island, Via) VALUES(?1, ?2, ?3, ?4, ?5)");
        auto literalInsert =
            db.Prepare("INSERT INTO LiteralRef(Site, Target, Caller, Kind, Value) VALUES(?1, ?2, ?3, ?4, ?5)");
        auto virtualCallInsert = db.Prepare(
            "INSERT INTO VirtualCall(Image, Site, Instruction, SlotOffset, Discriminator, Candidate, "
            "CandidateSymbol, Resolver) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
        for (const auto* statement : {&segmentInsert, &sectionInsert, &functionInsert, &symbolInsert, &callInsert,
                                      &literalInsert, &virtualCallInsert})
        {
            if (!*statement)
            {
                return std::unexpected(statement->error());
            }
        }

        for (const auto& segment : facts.Segments)
        {
            if (auto ok = segmentInsert->Bind(1, segment.Name); !ok) return ok;
            if (auto ok = segmentInsert->Bind(2, static_cast<std::int64_t>(segment.Address)); !ok) return ok;
            if (auto ok = segmentInsert->Bind(3, static_cast<std::int64_t>(segment.Size)); !ok) return ok;
            if (auto ok = segmentInsert->Step(); !ok) return std::unexpected(ok.error());
            if (auto ok = segmentInsert->Reset(); !ok) return ok;
        }
        for (const auto& section : facts.Sections)
        {
            if (auto ok = sectionInsert->Bind(1, section.FullName()); !ok) return ok;
            if (auto ok = sectionInsert->Bind(2, static_cast<std::int64_t>(section.Address)); !ok) return ok;
            if (auto ok = sectionInsert->Bind(3, static_cast<std::int64_t>(section.Size)); !ok) return ok;
            if (auto ok = sectionInsert->Step(); !ok) return std::unexpected(ok.error());
            if (auto ok = sectionInsert->Reset(); !ok) return ok;
        }
        for (const auto& function : facts.Functions)
        {
            if (auto ok = functionInsert->Bind(1, static_cast<std::int64_t>(function.Address)); !ok) return ok;
            if (auto ok = functionInsert->Bind(2, static_cast<std::int64_t>(function.Size)); !ok) return ok;
            if (auto ok = functionInsert->Step(); !ok) return std::unexpected(ok.error());
            if (auto ok = functionInsert->Reset(); !ok) return ok;
        }
        for (const auto& symbol : facts.Symbols)
        {
            const auto nameId = InternName(db, *nameLookup, *nameInsert, names, symbol.Name);
            if (!nameId) return std::unexpected(nameId.error());
            const auto demangled = demangle(symbol.Name);
            std::optional<std::int64_t> demangledId;
            if (demangled)
            {
                const auto id = InternName(db, *nameLookup, *nameInsert, names, *demangled);
                if (!id) return std::unexpected(id.error());
                demangledId = *id;
            }
            if (auto ok = symbolInsert->Bind(1, static_cast<std::int64_t>(symbol.Address)); !ok) return ok;
            if (auto ok = symbolInsert->Bind(2, *nameId); !ok) return ok;
            if (demangledId)
            {
                if (auto ok = symbolInsert->Bind(3, *demangledId); !ok) return ok;
            }
            else if (auto ok = symbolInsert->BindNull(3); !ok) return ok;
            if (auto ok = symbolInsert->Bind(4, static_cast<std::int64_t>(symbol.External ? 1 : 0)); !ok) return ok;
            if (auto ok = symbolInsert->Step(); !ok) return std::unexpected(ok.error());
            if (auto ok = symbolInsert->Reset(); !ok) return ok;
        }
        for (const auto& call : facts.Calls)
        {
            if (auto ok = callInsert->Bind(1, static_cast<std::int64_t>(call.Site)); !ok) return ok;
            if (auto ok = callInsert->Bind(2, static_cast<std::int64_t>(call.Caller)); !ok) return ok;
            if (auto ok = callInsert->Bind(3, static_cast<std::int64_t>(call.Target)); !ok) return ok;
            if (call.Island)
            {
                if (auto ok = callInsert->Bind(4, static_cast<std::int64_t>(*call.Island)); !ok) return ok;
            }
            else if (auto ok = callInsert->BindNull(4); !ok) return ok;
            if (auto ok = callInsert->Bind(5, call.Via); !ok) return ok;
            if (auto ok = callInsert->Step(); !ok) return std::unexpected(ok.error());
            if (auto ok = callInsert->Reset(); !ok) return ok;
        }
        for (const auto& literal : facts.Literals)
        {
            if (auto ok = literalInsert->Bind(1, static_cast<std::int64_t>(literal.Site)); !ok) return ok;
            if (auto ok = literalInsert->Bind(2, static_cast<std::int64_t>(literal.Target)); !ok) return ok;
            if (literal.Caller)
            {
                if (auto ok = literalInsert->Bind(3, static_cast<std::int64_t>(*literal.Caller)); !ok) return ok;
            }
            else if (auto ok = literalInsert->BindNull(3); !ok) return ok;
            if (auto ok = literalInsert->Bind(4, literal.Kind); !ok) return ok;
            if (literal.Value)
            {
                if (auto ok = literalInsert->Bind(5, *literal.Value); !ok) return ok;
            }
            else if (auto ok = literalInsert->BindNull(5); !ok) return ok;
            if (auto ok = literalInsert->Step(); !ok) return std::unexpected(ok.error());
            if (auto ok = literalInsert->Reset(); !ok) return ok;
        }
        for (const auto& vcall : facts.VirtualCalls)
        {
            if (auto ok = virtualCallInsert->Bind(1, facts.Path); !ok) return ok;
            if (auto ok = virtualCallInsert->Bind(2, static_cast<std::int64_t>(vcall.Site)); !ok) return ok;
            if (auto ok = virtualCallInsert->Bind(3, vcall.Instruction); !ok) return ok;
            if (auto ok = virtualCallInsert->Bind(4, static_cast<std::int64_t>(vcall.SlotOffset)); !ok) return ok;
            if (auto ok = virtualCallInsert->Bind(5, static_cast<std::int64_t>(vcall.Discriminator)); !ok) return ok;
            if (auto ok = virtualCallInsert->Bind(6, static_cast<std::int64_t>(vcall.Candidate)); !ok) return ok;
            if (auto ok = virtualCallInsert->Bind(7, vcall.CandidateSymbol); !ok) return ok;
            if (auto ok = virtualCallInsert->Bind(8, kVirtualCallResolver); !ok) return ok;
            if (auto ok = virtualCallInsert->Step(); !ok) return std::unexpected(ok.error());
            if (auto ok = virtualCallInsert->Reset(); !ok) return ok;
        }

        if (auto ok = tx->Commit(); !ok)
        {
            return ok;
        }
        return Store::CreateImageIndexes(db);
    }
}
