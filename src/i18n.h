#pragma once

// Traduction de l'interface.
//
// Les textes sont dans des fichiers src/lang/<groupe>.<code>.lang (UTF-8), intégrés à l'exécutable
// comme ressources de type « LANG ». Des fichiers .lang posés dans le dossier « lang » à côté de
// l'exe s'y ajoutent (nouvelle langue, ou correction d'une traduction existante).
//
// Format d'un fichier :
//     # commentaire
//     @code = fr
//     @name = Français
//     viewer.open = Ouvrir (Ctrl+O)
//     viewer.zoom = Zoom %d %%          <- spécificateurs printf autorisés (même ordre dans chaque langue)
// Échappements dans les valeurs : \n (retour à la ligne), \t, \\.
//
// Repli quand une clé manque : langue choisie, puis anglais, puis français, puis la clé elle-même.
// Les recherches sont sans verrou et utilisables depuis n'importe quel thread après Init().

#include <string>
#include <vector>

namespace i18n {

struct Language {
    std::wstring code;  // « fr », « en », « de »...
    std::wstring name;  // nom dans la langue elle-même : « Français », « Deutsch »...
};

// À appeler une fois au démarrage, avant tout appel à T(). preferred vide = langue de Windows.
void Init(const std::wstring& exeDir, const std::wstring& preferred);

std::vector<Language> Languages();     // langues disponibles, triées par nom
std::wstring CurrentCode();            // langue utilisée
std::wstring SystemCode();             // langue de Windows ramenée à une langue disponible
void SetLanguage(const std::wstring& code);  // vide = langue de Windows

}  // namespace i18n

// Texte traduit (pointeur valable jusqu'à la fin du programme).
const wchar_t* T(const char* key);

// Texte traduit utilisé comme format printf.
std::wstring Tf(const char* key, ...);

// Comme T(), mais renvoie fallback si la clé n'existe dans aucune langue (plugins tiers).
const wchar_t* TrOr(const char* key, const wchar_t* fallback);
