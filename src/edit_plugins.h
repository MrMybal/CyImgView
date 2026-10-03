#pragma once

// Plugins d'édition (include/cyimg_edit_plugin.h) : chargement, descriptions recopiées dans leur
// version courante, appels protégés, calculs process() sur des threads de travail.
//
// Chargés à la première utilisation (entrée en mode édition, menu « Ouvrir avec », aide) et pas au
// démarrage : les DLL sont déjà en mémoire (le décodeur les a ouvertes) ou petites, et rien ne les
// demande avant. Une fonction qui plante est désactivée pour la session (disabled).

#include "common.h"
#include "cyimg_edit_plugin.h"

#include <functional>
#include <type_traits>

class Viewer;
namespace editplug { struct Job; }

// Contexte des appels vers un plugin (opaque pour lui) : calcul process() sur un thread de travail
// (job), ou rappel sur le thread de l'interface (viewer, outil appelant, surcouche en cours).
struct CyImgEditCtx {
    editplug::Job* job = nullptr;
    Viewer* viewer = nullptr;
    int tool = -1;
    bool overlay = false;
};

namespace editplug {

// Plantage d'un plugin : code d'exception et adresse (« annotate.dll+0x1A2B, C0000005 »)
struct Crash {
    DWORD code = 0;
    void* addr = nullptr;
    std::wstring Where() const;
};

// Exécute f() sous protection SEH (accès mémoire invalide, exception C++ non rattrapée...).
// false : le plugin a planté, crash est rempli.
bool GuardedInvoke(void (*fn)(void*), void* arg, Crash& crash);
template <class F>
bool Guard(F&& f, Crash& crash) {
    return GuardedInvoke([](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); }, &f, crash);
}

// Texte traduit d'un plugin (clé facultative, texte par défaut)
std::wstring Text(const char* key, const wchar_t* def);

struct Plugin {
    HMODULE module = nullptr;
    CyImgEditPlugin api{};      // recopiée (champs inconnus à zéro)
    std::wstring file;          // nom de la DLL
    std::wstring Name() const;
};

// Paramètres normalisés (struct_size du plugin pris en compte), libellés et choix traduits
struct Param {
    CyImgParam p{};
    std::wstring label, unit;
    std::vector<std::wstring> choices;
};

struct Effect {
    const Plugin* plugin = nullptr;
    CyImgEffect api{};
    std::wstring name, category;
    std::vector<Param> params;
    bool disabled = false;
};

struct Tool {
    const Plugin* plugin = nullptr;
    CyImgTool api{};
    std::wstring name, hint, glyph;
    std::vector<Param> params;
    std::vector<double> values;  // valeurs courantes des paramètres (gardées pendant la session)
    bool disabled = false;
};

struct Mode {
    const Plugin* plugin = nullptr;
    CyImgMode api{};
    std::wstring name, hint, glyph;
    std::vector<Param> controls;
    bool disabled = false;
};

struct Action {
    const Plugin* plugin = nullptr;
    CyImgAction api{};
    std::wstring name;
    std::vector<std::wstring> exts;  // vide : tous les fichiers
    bool disabled = false;
};

struct Registry {
    bool loaded = false;
    std::vector<std::unique_ptr<Plugin>> plugins;
    std::vector<Effect> effects;
    std::vector<Tool> tools;
    std::vector<Mode> modes;
    std::vector<Action> actions;
};

Registry& Get();
// Chargement (une seule fois) des DLL du dossier plugins qui exportent CyImg_GetEditPlugin.
void Load(const std::wstring& exeDir, const CyImgEditHost* host);
std::vector<double> Defaults(const std::vector<Param>& params);
std::wstring GlyphString(uint32_t cp);  // caractère Unicode (paires de substitution au-delà de U+FFFF)

// --- Calculs process() sur un thread de travail ---------------------------------------------

struct Job {
    uint64_t id = 0;
    std::atomic<bool> cancel{false};
    // Entrée
    std::shared_ptr<PixelBuffer> src;  // pixels d'origine (partagés, lecture seule)
    uint32_t width = 0, height = 0, stride = 0;
    CyImgRect sel{};
    double scale = 1;
    uint32_t fullWidth = 0, fullHeight = 0;
    std::vector<double> values;
    int (*process)(const CyImgEditHost*, CyImgEditCtx*, CyImgImage*, const double*) = nullptr;
    // Résultat
    PixelBuffer dst;
    int result = CYIMG_E_FAIL;
    bool crashed = false;
    Crash crash;
    double ms = 0;
    std::function<void(Job&)> done;  // appelé sur le thread de l'interface
};
using JobPtr = std::shared_ptr<Job>;

// Calcul sur le thread appelant (intégration d'un mode : résultat attendu tout de suite)
void RunJob(Job& job, const CyImgEditHost* host);
// Lance le calcul ; à la fin, msg est posté à hwnd et DispatchJobs() appelle job->done.
void StartJob(const JobPtr& job, const CyImgEditHost* host, HWND hwnd, UINT msg);
void DispatchJobs();  // thread de l'interface, à la réception de msg

}  // namespace editplug
