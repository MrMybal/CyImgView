// Banc d'essai du décodage : cyimg_bench <fichier|dossier> [taille_vignette] [best]
// Décode chaque image en pleine résolution puis en vignette et affiche les temps.
// « best » force aussi la version complète (développement RAW) même si l'aperçu suffit.
#include "../src/decoder.h"
#include "../src/folder.h"
#include "../src/i18n.h"
#include "../src/save.h"
#include "../src/updater.h"
#include "../src/version.h"

#include <cstdio>

// cyimg_bench convert <source> <destination> [qualité] : conversion de format en ligne de commande
static int Convert(Decoder& dec, const std::wstring& in, const std::wstring& out, int quality) {
    std::wstring err;
    ImagePtr img = dec.Decode(in, 0, false, nullptr, err);
    if (img && img->refinable) {  // RAW : version développée
        if (ImagePtr best = dec.Decode(in, 0, true, nullptr, err)) img = best;
    }
    if (!img) {
        fwprintf(stderr, L"lecture impossible : %ls\n", err.c_str());
        return 1;
    }
    static const bool f[9] = {false, false, true, false, true, true, false, true, false};
    static const int r[9] = {0, 0, 0, 2, 2, 3, 1, 1, 3};
    uint32_t o = img->orientation <= 8 ? img->orientation : 1;
    PixelBuffer px;
    uint32_t w, h, stride;
    if (!OrientPixels(img->pixels.Data(), img->width, img->height, img->stride, f[o], r[o], px, w, h, stride)) return 1;
    auto formats = EnumerateSaveFormats();
    SaveRequest req;
    req.path = out;
    req.quality = quality;
    req.format = -1;
    std::wstring ext = ExtensionOf(out);
    for (int i = 0; i < int(formats.size()); ++i)
        for (auto& e : formats[i].exts)
            if (e == ext) req.format = i;
    if (req.format < 0) {
        fwprintf(stderr, L"format de sortie inconnu : .%ls\n", ext.c_str());
        return 1;
    }
    double t0 = NowSeconds();
    if (!SaveImage(req, formats[size_t(req.format)], px.Data(), w, h, stride, img->hasAlpha, err)) {
        fwprintf(stderr, L"échec : %ls\n", err.c_str());
        return 1;
    }
    wprintf(L"%ls -> %ls (%ls, %ux%u) en %.0f ms\n", FileNameOf(in).c_str(), FileNameOf(out).c_str(),
            formats[size_t(req.format)].name.c_str(), w, h, (NowSeconds() - t0) * 1000);
    return 0;
}

// Plantage non rattrapé (ex. dans un thread d'une bibliothèque) : affiche le module et le décalage,
// pour retrouver la fonction fautive avec un fichier .map.
static LONG WINAPI ReportCrash(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod))
        GetModuleFileNameW(mod, name, MAX_PATH);
    fwprintf(stderr, L"\nPLANTAGE %08lX dans %ls +0x%llX (thread %lu)\n", ep->ExceptionRecord->ExceptionCode, name,
             static_cast<unsigned long long>(static_cast<char*>(addr) - reinterpret_cast<char*>(mod)), GetCurrentThreadId());
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int wmain(int argc, wchar_t** argv) {
    AddVectoredExceptionHandler(1, ReportCrash);  // voit aussi les erreurs rattrapées plus tard
    if (argc < 2) {
        fwprintf(stderr, L"usage: cyimg_bench <fichier|dossier> [taille_vignette] [best]\n"
                         L"       cyimg_bench convert <source> <destination> [qualité]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    i18n::Init(ParentOf(exe), L"");  // messages du décodeur dans la langue de Windows
    Decoder dec;
    dec.Init(ParentOf(exe));
    if (!wcscmp(argv[1], L"update-check") && argc > 2) {  // test : update-check propriétaire/dépôt [version]
        ReleaseInfo rel;
        std::wstring err;
        UpdateStatus st = CheckForUpdate(argv[2], argc > 3 ? argv[3] : CYIMG_VERSION_WSTR, rel, err);
        wprintf(L"statut=%ls tag=%ls version=%ls\nasset=%ls (%llu octets)\nurl=%ls\nsha256=%ls\nerreur=%ls\n",
                st == UpdateStatus::Available ? L"disponible" : st == UpdateStatus::UpToDate ? L"à jour" : L"erreur",
                rel.tag.c_str(), rel.version.c_str(), rel.assetName.c_str(), rel.assetSize, rel.assetUrl.c_str(),
                rel.sha256.c_str(), err.c_str());
        if (st == UpdateStatus::Available && argc > 4) {  // + chemin : télécharge aussi l'archive
            bool ok = DownloadUpdate(rel, argv[4], [](uint64_t, uint64_t) { return true; }, err);
            wprintf(L"téléchargement : %ls %ls\n", ok ? L"OK" : L"ÉCHEC", err.c_str());
        }
        return st == UpdateStatus::Error ? 1 : 0;
    }
    if (!wcscmp(argv[1], L"update-install") && argc > 3) {  // test : update-install archive.zip dossier
        std::wstring err;
        bool ok = InstallUpdate(argv[2], argv[3], err);
        wprintf(L"installation : %ls %ls\n", ok ? L"OK" : L"ÉCHEC", err.c_str());
        return ok ? 0 : 1;
    }
    if (!wcscmp(argv[1], L"convert")) {
        if (argc < 4) return 2;
        int code = Convert(dec, argv[2], argv[3], argc > 4 ? _wtoi(argv[4]) : 92);
        Decoder::ReleaseThreadResources();
        CoUninitialize();
        return code;
    }
    for (auto& p : dec.Plugins()) wprintf(L"plugin: %ls %ls [%ls]\n", p.name.c_str(), p.version.c_str(), p.extensions.c_str());

    uint32_t thumb = argc > 2 ? uint32_t(_wtoi(argv[2])) : 160;
    std::vector<std::wstring> files;
    DWORD attr = GetFileAttributesW(argv[1]);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        for (auto& e : ListImages(argv[1], dec)) files.push_back(e.path);
    } else {
        files.push_back(argv[1]);
    }
    int failures = 0;
    for (auto& f : files) {
        std::wstring err;
        ImagePtr full = dec.Decode(f, 0, false, nullptr, err);
        if (!full) {
            wprintf(L"ECHEC %ls : %ls\n", FileNameOf(f).c_str(), err.c_str());
            ++failures;
            continue;
        }
        ImagePtr th = dec.Decode(f, thumb, false, nullptr, err);
        wprintf(L"%-40ls %5ux%-5u o=%u a=%d %-28ls full %6.1f ms | thumb %4ux%-4u %5.1f ms\n", FileNameOf(f).c_str(),
                full->width, full->height, full->orientation, full->hasAlpha, full->format.c_str(), full->decodeMs,
                th ? th->width : 0, th ? th->height : 0, th ? th->decodeMs : 0.0);
        if (full->refinable || (argc > 3 && !wcscmp(argv[3], L"best"))) {
            ImagePtr best = dec.Decode(f, 0, true, nullptr, err);
            if (best) wprintf(L"      -> affiné : %ux%u o=%u %ls en %.1f ms\n", best->width, best->height, best->orientation,
                              best->format.c_str(), best->decodeMs);
            else wprintf(L"      -> affinage en échec : %ls\n", err.c_str());
        }
        for (auto& [k, v] : full->info) wprintf(L"      %ls: %ls\n", k.c_str(), v.c_str());
    }
    Decoder::ReleaseThreadResources();
    CoUninitialize();
    return failures ? 1 : 0;
}
