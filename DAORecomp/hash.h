#pragma once
// Hashing files with Windows' BCrypt (the updater's downloads, the title update).

#include <span>

namespace dao
{
    // Hashes an open file from where it is to its end with a BCrypt algorithm
    // (BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM) into `digest`, which must be
    // that algorithm's size; `size` is set to the bytes read. false if it can't.
    bool HashFile(HANDLE file, const wchar_t* algorithm, std::span<uint8_t> digest, uint64_t& size);

    // The SHA-1 of a file as upper-case hex, or "" if it can't be read.
    std::string FileSha1(const std::filesystem::path& path);
}
