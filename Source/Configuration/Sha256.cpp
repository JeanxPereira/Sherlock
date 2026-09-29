// Sherlock — Source/Configuration/Sha256.cpp
// SHA-256 through Windows CNG: the digest a Documents.db records of the sherlock.json it was built from.
#include <Configuration/Config.h>

#include <windows.h>

#include <bcrypt.h>

#include <array>

namespace Sherlock::Configuration
{
    using Foundation::DiagnosticCode;
    using Foundation::Fail;
    using Foundation::Severity;

    Foundation::Expected<std::string> Sha256Hex(std::string_view bytes)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        {
            return Fail(DiagnosticCode::Unsupported, Severity::NotVerified, "Configuration::Sha256Hex", "CNG",
                        "the SHA-256 provider cannot be opened", "check the Windows cryptography services");
        }
        std::array<UCHAR, 32> digest{};
        BCRYPT_HASH_HANDLE    hash   = nullptr;
        NTSTATUS              status = ::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
        if (BCRYPT_SUCCESS(status))
        {
            status = ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                      static_cast<ULONG>(bytes.size()), 0);
        }
        if (BCRYPT_SUCCESS(status))
        {
            status = ::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        }
        if (hash != nullptr) ::BCryptDestroyHash(hash);
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!BCRYPT_SUCCESS(status))
        {
            return Fail(DiagnosticCode::Unsupported, Severity::NotVerified, "Configuration::Sha256Hex", "CNG",
                        "the SHA-256 digest cannot be computed", "check the Windows cryptography services");
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string text;
        text.reserve(64);
        for (const UCHAR byte : digest)
        {
            text.push_back(kHex[byte >> 4]);
            text.push_back(kHex[byte & 0x0F]);
        }
        return text;
    }
}
