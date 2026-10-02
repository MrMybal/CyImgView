# OpenEXR avec la bibliothèque C OpenEXRCore (third_party/openexr, BSD-3)
#  + libdeflate (third_party/libdeflate, MIT) : compressions ZIP/ZIPS
#  + zstd (third_party/openexr/external/zstd, BSD) : compression ZSTD (OpenEXR 3.4+)
#  + OpenJPH (third_party/openexr/external/OpenJPH, BSD-2) : compressions HTJ2K (OpenEXR 3.4+)
#  + half.h d'Imath (third_party/Imath, BSD-3), utilisé par le code HTJ2K
# Les en-têtes de configuration générés d'ordinaire par CMake sont écrits à la main dans
# plugins/exr/config.
set(EXR_ROOT ${CMAKE_SOURCE_DIR}/third_party/openexr)
set(EXR_CORE ${EXR_ROOT}/src/lib/OpenEXRCore)
set(EXR_CONFIG ${CYIMG_PLUGIN_DIR}/config)

# --- libdeflate ---
set(DEFLATE_DIR ${CMAKE_SOURCE_DIR}/third_party/libdeflate)
add_library(cyimg_exr_deflate STATIC
    ${DEFLATE_DIR}/lib/utils.c ${DEFLATE_DIR}/lib/x86/cpu_features.c ${DEFLATE_DIR}/lib/arm/cpu_features.c
    ${DEFLATE_DIR}/lib/deflate_compress.c ${DEFLATE_DIR}/lib/deflate_decompress.c ${DEFLATE_DIR}/lib/adler32.c
    ${DEFLATE_DIR}/lib/zlib_compress.c ${DEFLATE_DIR}/lib/zlib_decompress.c)
target_include_directories(cyimg_exr_deflate PUBLIC ${DEFLATE_DIR})
target_compile_options(cyimg_exr_deflate PRIVATE /W0)

# --- zstd (sans assembleur sous MSVC) ---
set(ZSTD_DIR ${EXR_ROOT}/external/zstd/lib)
file(GLOB CYIMG_ZSTD_SOURCES CONFIGURE_DEPENDS ${ZSTD_DIR}/common/*.c ${ZSTD_DIR}/compress/*.c ${ZSTD_DIR}/decompress/*.c)
add_library(cyimg_exr_zstd STATIC ${CYIMG_ZSTD_SOURCES})
target_include_directories(cyimg_exr_zstd PUBLIC ${ZSTD_DIR})
target_compile_definitions(cyimg_exr_zstd PRIVATE ZSTD_DISABLE_ASM)
target_compile_options(cyimg_exr_zstd PRIVATE /W0)

# --- OpenJPH (noyau seul ; les fichiers SIMD sont choisis à l'exécution selon le processeur) ---
set(OJPH_DIR ${EXR_ROOT}/external/OpenJPH/src/core)
file(GLOB CYIMG_OJPH_SOURCES CONFIGURE_DEPENDS ${OJPH_DIR}/codestream/*.cpp ${OJPH_DIR}/coding/*.cpp
    ${OJPH_DIR}/others/*.cpp ${OJPH_DIR}/others/*.c ${OJPH_DIR}/transform/*.cpp)
add_library(cyimg_exr_openjph STATIC ${CYIMG_OJPH_SOURCES})
target_include_directories(cyimg_exr_openjph PUBLIC ${OJPH_DIR} PRIVATE ${OJPH_DIR}/openjph)
target_compile_definitions(cyimg_exr_openjph PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(cyimg_exr_openjph PRIVATE /W0 /EHsc)
foreach(f ${CYIMG_OJPH_SOURCES})
    if(f MATCHES "_avx512\\.cpp$")
        set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    elseif(f MATCHES "_avx2\\.cpp$")
        set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    elseif(f MATCHES "_avx\\.cpp$")
        set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "/arch:AVX")
    endif()
endforeach()
# Code AVX/AVX2/AVX-512 par fichier : pas d'optimisation globale qui pourrait le mélanger au reste
set_target_properties(cyimg_exr_openjph PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# --- OpenEXRCore ---
file(GLOB CYIMG_EXRCORE_SOURCES CONFIGURE_DEPENDS ${EXR_CORE}/*.c ${EXR_CORE}/*.cpp)
add_library(cyimg_openexrcore STATIC ${CYIMG_EXRCORE_SOURCES})
target_include_directories(cyimg_openexrcore PUBLIC ${EXR_CORE} ${EXR_CONFIG}
    PRIVATE ${EXR_CONFIG}/Imath ${CMAKE_SOURCE_DIR}/third_party/Imath/src/Imath)
target_compile_definitions(cyimg_openexrcore PRIVATE IMATH_HALF_NO_LOOKUP_TABLE _CRT_SECURE_NO_WARNINGS)
target_compile_options(cyimg_openexrcore PRIVATE /W0 /EHsc)
target_link_libraries(cyimg_openexrcore PUBLIC cyimg_exr_deflate cyimg_exr_zstd cyimg_exr_openjph)

cyimg_add_plugin(exr ${CYIMG_PLUGIN_DIR}/exr.cpp)
target_link_libraries(exr PRIVATE cyimg_openexrcore)
