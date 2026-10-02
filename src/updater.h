#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Mises à jour via GitHub Releases :
//  1. GET https://api.github.com/repos/<dépôt>/releases/latest
//  2. version de l'étiquette (vX.Y.Z) comparée à CYIMG_VERSION
//  3. téléchargement du zip « *win64*.zip » de la release (contrôle SHA-256 si GitHub le fournit)
//  4. remplacement des fichiers : chaque fichier existant est renommé en .old (possible même pour
//     l'exe et les DLL en cours d'utilisation), le nouveau prend sa place ; retour arrière en cas
//     d'échec. Les .old sont supprimés au démarrage suivant.

struct ReleaseInfo {
    std::wstring tag, version, name, notes, page;
    std::wstring assetName, assetUrl;
    uint64_t assetSize = 0;
    std::wstring sha256;  // hexadécimal minuscule, vide si GitHub ne le fournit pas
};

enum class UpdateStatus { Error, UpToDate, Available };

UpdateStatus CheckForUpdate(const std::wstring& repo, const std::wstring& currentVersion, ReleaseInfo& out,
                            std::wstring& error);

// <0 si a < b, 0 si égales, >0 si a > b ("v1.10.0" > "1.9.3")
int CompareVersions(const std::wstring& a, const std::wstring& b);

// progress(octets reçus, total) ; retourner false annule.
bool DownloadUpdate(const ReleaseInfo& rel, const std::wstring& destFile,
                    const std::function<bool(uint64_t, uint64_t)>& progress, std::wstring& error);

// Extrait le zip et remplace les fichiers de appDir. L'application doit ensuite redémarrer.
bool InstallUpdate(const std::wstring& zipFile, const std::wstring& appDir, std::wstring& error);

// Supprime les restes d'une mise à jour précédente (*.old, *.new).
void CleanupUpdateLeftovers(const std::wstring& appDir);
