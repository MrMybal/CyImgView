#pragma once

// Version de CyImgView : seule source de vÃ©ritÃ© (ressource .rc, mises Ã  jour, script de packaging).
// Les releases GitHub doivent Ãªtre Ã©tiquetÃ©es Â« vMAJOR.MINOR.PATCH Â».
#define CYIMG_VERSION_MAJOR 1
#define CYIMG_VERSION_MINOR 8
#define CYIMG_VERSION_PATCH 2

#define CYIMG_STR2(x) #x
#define CYIMG_STR(x) CYIMG_STR2(x)
#define CYIMG_VERSION_STR CYIMG_STR(CYIMG_VERSION_MAJOR) "." CYIMG_STR(CYIMG_VERSION_MINOR) "." CYIMG_STR(CYIMG_VERSION_PATCH)
#ifndef RC_INVOKED
#define CYIMG_WSTR2(x) L##x
#define CYIMG_WSTR(x) CYIMG_WSTR2(x)
#define CYIMG_VERSION_WSTR CYIMG_WSTR(CYIMG_VERSION_STR)

// DÃ©pÃ´t GitHub qui publie les releases, au format "propriÃ©taire/dÃ©pÃ´t".
// Vide : la recherche de mises Ã  jour est dÃ©sactivÃ©e.
#define CYIMG_UPDATE_REPO L"MrMybal/CyImgView"
#endif
