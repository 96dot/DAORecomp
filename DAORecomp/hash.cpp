#include "stdafx.h"
#include "hash.h"

#include <bcrypt.h>

namespace dao
{
    bool HashFile(HANDLE file, const wchar_t* algorithm, std::span<uint8_t> digest, uint64_t& size)
    {
        BCRYPT_ALG_HANDLE provider = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr, 0) >= 0 &&
                  BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        if (ok)
        {
            std::vector<uint8_t> buffer(1 << 20);
            size = 0;
            DWORD read = 0;
            while ((ok = ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr)) && read > 0)
            {
                size += read;
                if (BCryptHashData(hash, buffer.data(), read, 0) < 0)
                {
                    ok = false;
                    break;
                }
            }
            ok = ok && BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) >= 0;
        }
        if (hash)
            BCryptDestroyHash(hash);
        if (provider)
            BCryptCloseAlgorithmProvider(provider, 0);
        return ok;
    }

    namespace
    {
        std::string Hex(std::span<const uint8_t> digest)
        {
            std::string hex;
            for (const uint8_t b : digest)
                hex += fmt::format("{:02X}", b);
            return hex;
        }
    }

    std::string FileSha1(const std::filesystem::path& path)
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {};
        std::array<uint8_t, 20> digest;
        uint64_t size = 0;
        const bool ok = HashFile(file, BCRYPT_SHA1_ALGORITHM, digest, size);
        CloseHandle(file);
        return ok ? Hex(digest) : std::string();
    }

    std::string Sha1(std::span<const uint8_t> data)
    {
        std::array<uint8_t, 20> digest;
        // (BCryptHash takes a ULONG length.)
        if (data.size() > ULONG_MAX ||
            BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data.data()), ULONG(data.size()),
                digest.data(), ULONG(digest.size())) < 0)
            return {};
        return Hex(digest);
    }
}
