// Sherlock — Source/SherlockCli/Demangler.cpp
// LoadLibraryW/GetProcAddress/FreeLibrary behind Demangler.
#include <SherlockCli/Demangler.h>

#include <windows.h>

#include <cstdlib>
#include <utility>
#include <vector>

namespace Sherlock::Cli
{
    std::filesystem::path DefaultDemanglerPath()
    {
        if (const char* env = std::getenv("SHERLOCK_DEMANGLER"); env != nullptr)
        {
            return env;
        }
        return LR"(C:\Program Files\IDA Professional 9.2\libSwiftDemangle.dll)";
    }

    std::optional<Demangler> Demangler::Load(const std::filesystem::path& dll)
    {
        HMODULE module = ::LoadLibraryW(dll.c_str());
        if (module == nullptr)
        {
            return std::nullopt;
        }
        auto fn = reinterpret_cast<DemangleFn>(::GetProcAddress(module, "swift_demangle_getDemangledName"));
        if (fn == nullptr)
        {
            ::FreeLibrary(module);
            return std::nullopt;
        }
        return Demangler(reinterpret_cast<HINSTANCE__*>(module), fn);
    }

    Demangler::Demangler(Demangler&& other) noexcept
        : _module(std::exchange(other._module, nullptr)), _fn(std::exchange(other._fn, nullptr))
    {
    }

    Demangler& Demangler::operator=(Demangler&& other) noexcept
    {
        if (this != &other)
        {
            if (_module != nullptr)
            {
                ::FreeLibrary(reinterpret_cast<HMODULE>(_module));
            }
            _module = std::exchange(other._module, nullptr);
            _fn     = std::exchange(other._fn, nullptr);
        }
        return *this;
    }

    Demangler::~Demangler()
    {
        if (_module != nullptr)
        {
            ::FreeLibrary(reinterpret_cast<HMODULE>(_module));
        }
    }

    std::optional<std::string> Demangler::Demangle(std::string_view symbol) const
    {
        std::string mangled(symbol);
        if (!mangled.empty() && mangled.front() == '_')
        {
            mangled.erase(0, 1);
        }
        std::vector<char> buffer(1024);
        std::size_t       length = _fn(mangled.c_str(), buffer.data(), buffer.size());
        if (length == 0)
        {
            return std::nullopt;
        }
        if (length > buffer.size())
        {
            buffer.assign(length, '\0');
            length = _fn(mangled.c_str(), buffer.data(), buffer.size());
            if (length == 0)
            {
                return std::nullopt;
            }
        }
        return std::string(buffer.data(), length);
    }
}
