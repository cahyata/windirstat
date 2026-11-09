// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

// Uniform Segoe Fluent glyphs the glass lists draw in place of the shell's per-type icons.
// Files fall into a handful of families, each with its own muted accent colour.

namespace GlassGlyphs
{
    struct Glyph
    {
        wchar_t code;
        COLORREF color;
    };

    inline constexpr Glyph Folder{ 0xE8B7, RGB(230, 168, 40) };
    inline constexpr Glyph Drive{ 0xEDA2, RGB(110, 120, 140) };
    inline constexpr Glyph Computer{ 0xE7F4, RGB(110, 120, 140) };

    // Pass the extension with or without its leading dot; case is ignored
    inline Glyph ForExtension(std::wstring_view extension)
    {
        if (!extension.empty() && extension.front() == L'.') extension.remove_prefix(1);
        wchar_t lower[16]{};
        if (extension.empty() || extension.size() >= std::size(lower)) return { 0xE8A5, RGB(120, 132, 160) };
        for (size_t i = 0; i < extension.size(); ++i)
            lower[i] = extension[i] >= L'A' && extension[i] <= L'Z' ? extension[i] + (L'a' - L'A') : extension[i];
        const std::wstring_view e(lower, extension.size());

        const auto in = [&](std::initializer_list<std::wstring_view> list) { return std::ranges::find(list, e) != list.end(); };

        if (in({ L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"tif", L"tiff", L"heic", L"avif", L"ico", L"svg", L"raw", L"psd" }))
            return { 0xE91B, RGB(52, 168, 110) };
        if (in({ L"mp3", L"wav", L"flac", L"m4a", L"aac", L"ogg", L"opus", L"wma", L"mid", L"midi" }))
            return { 0xE8D6, RGB(214, 84, 140) };
        if (in({ L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"flv", L"m4v", L"mpg", L"mpeg", L"ts" }))
            return { 0xE714, RGB(150, 98, 220) };
        if (in({ L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"zst", L"cab", L"iso", L"img", L"wim", L"vhd", L"vhdx" }))
            return { 0xF012, RGB(176, 124, 70) };
        if (in({ L"exe", L"msi", L"dll", L"sys", L"com", L"scr", L"msix", L"appx", L"bat", L"cmd", L"ps1" }))
            return { 0xECAA, RGB(66, 126, 232) };
        if (in({ L"c", L"cpp", L"cc", L"h", L"hpp", L"cs", L"java", L"js", L"jsx", L"tsx", L"py", L"rs", L"go", L"php", L"rb",
                 L"html", L"htm", L"css", L"json", L"xml", L"yml", L"yaml", L"sql", L"sh", L"lua", L"kt", L"swift" }))
            return { 0xE943, RGB(34, 160, 170) };
        return { 0xE8A5, RGB(120, 132, 160) };
    }
}
