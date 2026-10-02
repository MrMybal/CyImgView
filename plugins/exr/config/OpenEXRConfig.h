// Écrit à la main (remplace OpenEXRConfig.h généré par le CMake d'OpenEXR) : OpenEXRCore en
// bibliothèque statique, Windows x64, MSVC.
#ifndef INCLUDED_OPENEXR_CONFIG_H
#define INCLUDED_OPENEXR_CONFIG_H 1
#pragma once

#ifndef INCLUDED_OPENEXR_VERSION_H
#define OPENEXR_VERSION_MAJOR 3
#define OPENEXR_VERSION_MINOR 5
#define OPENEXR_VERSION_PATCH 1
#define OPENEXR_SOVERSION 33
#endif

#define OPENEXR_VERSION_STRING "3.5.1"
#define OPENEXR_PACKAGE_STRING "OpenEXR 3.5.1"
#define OPENEXR_VERSION_RELEASE_TYPE ""
#define OPENEXR_VERSION_EXTRA ""
#define OPENEXR_LIB_VERSION_STRING "33.3.5.1"

#define OPENEXR_VERSION_HEX                                                    \
    (((OPENEXR_VERSION_MAJOR) << 24) | ((OPENEXR_VERSION_MINOR) << 16) |       \
     ((OPENEXR_VERSION_PATCH) << 8))

#ifndef __has_attribute
#    define __has_attribute(x) 0
#endif

// Pas de visibilité de symboles sous MSVC (bibliothèque statique)
#define OPENEXR_EXPORT
#define OPENEXR_HIDDEN
#define OPENEXR_EXPORT_TYPE
#define OPENEXR_EXPORT_EXTERN_TEMPLATE
#define OPENEXR_EXPORT_ENUM
#define OPENEXR_EXPORT_TEMPLATE_TYPE
#define OPENEXR_EXPORT_TEMPLATE_INSTANCE

#define OPENEXR_DEPRECATED(msg) __declspec(deprecated (msg))

#endif
