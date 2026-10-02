#pragma once

#include "common.h"

class Decoder;

struct FileEntry {
    std::wstring path, name;
    uint64_t size = 0;
    FILETIME modified{};
};

// Liste les images reconnues d'un dossier, triées comme l'Explorateur (tri « naturel »).
std::vector<FileEntry> ListImages(const std::wstring& folder, const Decoder& decoder);
