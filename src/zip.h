#pragma once

// Lecture d'archives zip (méthodes « stocké » et deflate), sans dépendance.
// Utilisé par les mises à jour, l'installeur et les plugins (KRA, ORA, CDR...).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ZipEntryInfo {
    std::string name;       // chemin dans l'archive, séparateurs '/'
    uint16_t method = 0;    // 0 stocké, 8 deflate
    uint32_t compressedSize = 0, size = 0;
    size_t localHeader = 0; // position de l'en-tête local
};

// Liste les fichiers (pas les dossiers) d'une archive en mémoire.
bool ZipList(const uint8_t* data, size_t size, std::vector<ZipEntryInfo>& out);

// Décompresse une entrée.
bool ZipExtract(const uint8_t* data, size_t size, const ZipEntryInfo& e, std::vector<uint8_t>& out);

// Raccourci : extrait le fichier « name » (comparaison sans casse). false s'il est absent.
bool ZipExtractByName(const uint8_t* data, size_t size, const std::string& name, std::vector<uint8_t>& out);
