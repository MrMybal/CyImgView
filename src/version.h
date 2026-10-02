#pragma once

// Version de CyImgView : seule source de vérité (ressource .rc, mises à jour, script de packaging).
// Les releases GitHub doivent être étiquetées « vMAJOR.MINOR.PATCH ».
#define CYIMG_VERSION_MAJOR 1
#define CYIMG_VERSION_MINOR 4
#define CYIMG_VERSION_PATCH 1

#define CYIMG_STR2(x) #x
#define CYIMG_STR(x) CYIMG_STR2(x)
#define CYIMG_VERSION_STR CYIMG_STR(CYIMG_VERSION_MAJOR) "." CYIMG_STR(CYIMG_VERSION_MINOR) "." CYIMG_STR(CYIMG_VERSION_PATCH)
#ifndef RC_INVOKED
#define CYIMG_WSTR2(x) L##x
#define CYIMG_WSTR(x) CYIMG_WSTR2(x)
#define CYIMG_VERSION_WSTR CYIMG_WSTR(CYIMG_VERSION_STR)

// Dépôt GitHub qui publie les releases, au format "propriétaire/dépôt".
// Vide : la recherche de mises à jour est désactivée.
#define CYIMG_UPDATE_REPO L"MrMybal/CyImgView"
#endif
