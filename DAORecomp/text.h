#pragma once
// Text helpers. Paths go to and from UTF-8 here rather than with path::string(),
// which is in the ANSI code page and throws for the characters it lacks.

namespace dao
{
    // UTF-16 as UTF-8 (unpaired surrogates, which Windows names may have, become U+FFFD).
    inline std::string WideToUtf8(std::wstring_view text)
    {
        if (text.empty())
            return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    // Text in `codePage` as UTF-16 (invalid bytes become U+FFFD).
    inline std::wstring ToWide(std::string_view text, UINT codePage = CP_UTF8)
    {
        if (text.empty())
            return {};
        const int size = MultiByteToWideChar(codePage, 0, text.data(), int(text.size()), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(codePage, 0, text.data(), int(text.size()), out.data(), size);
        return out;
    }

    inline bool IsUtf8(std::string_view text)
    {
        return text.empty() ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0) > 0;
    }

    inline std::string Utf8(const std::filesystem::path& path)
    {
        return WideToUtf8(path.native());
    }

    inline std::filesystem::path FromUtf8(std::string_view text)
    {
        return ToWide(text);
    }

    // ASCII lowercase.
    inline std::string Lower(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
            c = char(tolower(uint8_t(c)));
        return out;
    }
}
