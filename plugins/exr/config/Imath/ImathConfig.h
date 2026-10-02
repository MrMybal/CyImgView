// Écrit à la main (remplace ImathConfig.h généré par le CMake d'Imath) : seul half.h est utilisé
#ifndef INCLUDED_IMATH_CONFIG_H
#define INCLUDED_IMATH_CONFIG_H 1
#pragma once

#define IMATH_INTERNAL_NAMESPACE_CUSTOM 0
#define IMATH_INTERNAL_NAMESPACE Imath_3_2
#define IMATH_NAMESPACE_CUSTOM 0
#define IMATH_NAMESPACE Imath

#define IMATH_VERSION_STRING "3.2.3"
#define IMATH_PACKAGE_STRING "Imath 3.2.3"
#define IMATH_VERSION_MAJOR 3
#define IMATH_VERSION_MINOR 2
#define IMATH_VERSION_PATCH 3
#define IMATH_VERSION_RELEASE_TYPE ""
#define IMATH_VERSION_HEX                                                      \
    ((uint32_t (IMATH_VERSION_MAJOR) << 24) |                                  \
     (uint32_t (IMATH_VERSION_MINOR) << 16) |                                  \
     (uint32_t (IMATH_VERSION_PATCH) << 8))
#define IMATH_LIB_VERSION_STRING "30.3.2.3"

#define IMATH_USE_NOEXCEPT 1
#define IMATH_NOEXCEPT noexcept

#ifndef IMATH_FOREIGN_VECTOR_INTEROP
#    define IMATH_FOREIGN_VECTOR_INTEROP 1
#endif
#define IMATH_HOSTDEVICE
#define IMATH_LIKELY(x) (x)
#define IMATH_UNLIKELY(x) (x)
#ifndef __has_attribute
#    define __has_attribute(x) 0
#endif
#define IMATH_DEPRECATED(msg) __declspec(deprecated (msg))
#endif
