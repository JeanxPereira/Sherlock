// Sherlock — tools/Sherlock/Source/SherlockCli/include/SherlockCli/Demangler.h
// libSwiftDemangle.dll loaded at run time, RAII over the module handle (derived).
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct HINSTANCE__;

namespace Sherlock::Cli
{
    class Demangler
    {
    public:
        static std::optional<Demangler> Load(const std::filesystem::path& dll);

        Demangler(Demangler&& other) noexcept;
        Demangler& operator=(Demangler&& other) noexcept;
        Demangler(const Demangler&)            = delete;
        Demangler& operator=(const Demangler&) = delete;
        ~Demangler();

        // NULL only when the export could not be found; never a status of a failed build.
        std::optional<std::string> Demangle(std::string_view symbol) const;

    private:
        using DemangleFn = std::size_t (*)(const char*, char*, std::size_t);
        Demangler(HINSTANCE__* module, DemangleFn fn) noexcept : _module(module), _fn(fn) {}

        HINSTANCE__* _module = nullptr;
        DemangleFn   _fn     = nullptr;
    };

    std::filesystem::path DefaultDemanglerPath();
}
