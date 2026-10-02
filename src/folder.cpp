#include "folder.h"

#include "decoder.h"

#include <shlwapi.h>

#include <algorithm>

std::vector<FileEntry> ListImages(const std::wstring& folder, const Decoder& decoder) {
    std::vector<FileEntry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        std::wstring name = fd.cFileName;
        if (!decoder.IsSupportedExt(ExtensionOf(name))) continue;
        if (!decoder.AcceptsFile(folder + L"\\" + name)) continue;  // ex. .uasset qui n'est pas une texture
        FileEntry e;
        e.path = folder + L"\\" + name;
        e.name = std::move(name);
        e.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.modified = fd.ftLastWriteTime;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(),
              [](const FileEntry& a, const FileEntry& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}
