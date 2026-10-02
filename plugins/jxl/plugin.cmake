# JPEG XL : libjxl (BSD-3, décodeur seul) + highway (Apache-2.0) dans third_party/
set(JXL_DIR ${CMAKE_SOURCE_DIR}/third_party/libjxl)
set(HWY_DIR ${CMAKE_SOURCE_DIR}/third_party/highway)
set(JXL_GEN_DIR ${CMAKE_BINARY_DIR}/jxl_gen/include)

# Cibles SIMD compilées (dispatch à l'exécution) : SSE2 (base x64), SSE4, AVX2. highway désactive
# lui-même AVX-512 sous MSVC (HWY_BROKEN_MSVC : génération de code erronée) ; SSSE3 et EMU128
# n'apportent rien et grossiraient la DLL.
set(CYIMG_HWY_DISABLED "HWY_DISABLED_TARGETS=(HWY_SSSE3|HWY_AVX3_DL|HWY_AVX3_ZEN4|HWY_AVX3_SPR|HWY_EMU128)")
set(CYIMG_THIRD_PARTY_FLAGS /W0 /EHsc /Gy /GR- /bigobj)

# --- highway ---
add_library(cyimg_hwy STATIC
    ${HWY_DIR}/hwy/abort.cc
    ${HWY_DIR}/hwy/aligned_allocator.cc
    ${HWY_DIR}/hwy/per_target.cc
    ${HWY_DIR}/hwy/print.cc
    ${HWY_DIR}/hwy/targets.cc
    ${HWY_DIR}/hwy/timer.cc)
target_include_directories(cyimg_hwy SYSTEM PUBLIC ${HWY_DIR})
target_compile_definitions(cyimg_hwy PUBLIC HWY_STATIC_DEFINE "${CYIMG_HWY_DISABLED}" PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(cyimg_hwy PRIVATE ${CYIMG_THIRD_PARTY_FLAGS})

# --- libjxl (décodeur + jxl_threads) ---
# En-têtes normalement produits par le CMake de libjxl
set(JPEGXL_MAJOR_VERSION 0)
set(JPEGXL_MINOR_VERSION 12)
set(JPEGXL_PATCH_VERSION 0)
configure_file(${JXL_DIR}/lib/jxl/version.h.in ${JXL_GEN_DIR}/jxl/version.h @ONLY)
foreach(exp jxl_export:JXL jxl_threads_export:JXL_THREADS jxl_cms_export:JXL_CMS)
    string(REPLACE ":" ";" exp ${exp})
    list(GET exp 0 file)
    list(GET exp 1 pfx)
    set(content "/* Généré par plugins/jxl/plugin.cmake : bibliothèque statique, aucun export. */
#pragma once
#define ${pfx}_EXPORT
#define ${pfx}_NO_EXPORT
#define ${pfx}_DEPRECATED __declspec(deprecated)
#define ${pfx}_DEPRECATED_EXPORT ${pfx}_DEPRECATED
#define ${pfx}_DEPRECATED_NO_EXPORT ${pfx}_DEPRECATED
")
    if(EXISTS ${JXL_GEN_DIR}/jxl/${file}.h)
        file(READ ${JXL_GEN_DIR}/jxl/${file}.h old)
    endif()
    if(NOT old STREQUAL content)
        file(WRITE ${JXL_GEN_DIR}/jxl/${file}.h "${content}")
    endif()
    unset(old)
endforeach()

file(GLOB_RECURSE JXL_DEC_SOURCES CONFIGURE_DEPENDS ${JXL_DIR}/lib/jxl/*.cc)
add_library(cyimg_jxl_dec STATIC ${JXL_DEC_SOURCES} ${JXL_DIR}/lib/threads/resizable_parallel_runner.cc)
target_include_directories(cyimg_jxl_dec SYSTEM PUBLIC ${JXL_GEN_DIR} ${JXL_DIR}/lib/include PRIVATE ${JXL_DIR})
target_compile_definitions(cyimg_jxl_dec
    PUBLIC JXL_STATIC_DEFINE JXL_THREADS_STATIC_DEFINE JXL_CMS_STATIC_DEFINE
    PRIVATE JXL_INTERNAL_LIBRARY_BUILD JPEGXL_ENABLE_TRANSCODE_JPEG=0 JPEGXL_ENABLE_BOXES=0
            JPEGXL_ENABLE_SKCMS=0 _CRT_SECURE_NO_WARNINGS)
target_compile_options(cyimg_jxl_dec PRIVATE ${CYIMG_THIRD_PARTY_FLAGS})
target_link_libraries(cyimg_jxl_dec PUBLIC cyimg_hwy)

cyimg_add_plugin(jxl ${CYIMG_PLUGIN_DIR}/jxl.cpp)
target_link_libraries(jxl PRIVATE cyimg_jxl_dec windowscodecs ole32)
target_link_options(jxl PRIVATE /OPT:REF /OPT:ICF)
