cmake_minimum_required(VERSION 3.20)
project(CyImgView LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CRT statique : un seul .exe, aucun redistribuable à installer
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
set(CYIMG_WARNINGS /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)

set(OUT_DIR ${CMAKE_BINARY_DIR}/bin)

add_executable(CyImgView WIN32
    src/main.cpp
    src/viewer.cpp
    src/decoder.cpp
    src/loader.cpp
    src/folder.cpp
    src/save.cpp
    src/viewer_edit.cpp
    src/viewer_update.cpp
    src/viewer_settings.cpp
    src/updater.cpp
    src/inflate.cpp
    src/app.rc
    src/app.manifest)
target_compile_options(CyImgView PRIVATE ${CYIMG_WARNINGS})
target_include_directories(CyImgView PRIVATE include)
target_link_libraries(CyImgView PRIVATE d2d1 d3d11 dxgi dwrite windowscodecs shlwapi dwmapi shell32 ole32 propsys comctl32 comdlg32 winhttp bcrypt)
set_target_properties(CyImgView PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})

# Banc d'essai console du décodage (mesure des temps, sans interface)
add_executable(cyimg_bench tools/bench.cpp src/decoder.cpp src/folder.cpp src/save.cpp src/updater.cpp src/inflate.cpp)
target_compile_options(cyimg_bench PRIVATE ${CYIMG_WARNINGS})
target_include_directories(cyimg_bench PRIVATE include)
target_link_libraries(cyimg_bench PRIVATE windowscodecs shlwapi ole32 propsys winhttp bcrypt)
set_target_properties(cyimg_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})

# --- LibRaw (third_party/LibRaw, LGPL 2.1 / CDDL 1.0) ----------------------------------------
set(LIBRAW_DIR ${CMAKE_SOURCE_DIR}/third_party/LibRaw)
file(GLOB_RECURSE LIBRAW_SOURCES CONFIGURE_DEPENDS ${LIBRAW_DIR}/src/*.cpp)
list(FILTER LIBRAW_SOURCES EXCLUDE REGEX "/src/integration/|_ph\.cpp$")  # *_ph.cpp : agrégats (#include) des autres sources
add_library(libraw STATIC ${LIBRAW_SOURCES})
target_include_directories(libraw PUBLIC ${LIBRAW_DIR})
target_compile_definitions(libraw PUBLIC LIBRAW_NODLL PRIVATE LIBRAW_BUILDLIB _CRT_SECURE_NO_WARNINGS)
# OpenMP : dématriçage et décodage multithreads (nécessite vcomp140.dll, copiée à côté du plugin)
target_compile_options(libraw PRIVATE /W0 /EHsc /openmp /bigobj)
set_target_properties(libraw PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# --- Plugins ---------------------------------------------------------------------------------
add_library(raw SHARED plugins/raw/raw.cpp plugins/raw/previews.cpp)
target_compile_options(raw PRIVATE /utf-8 /W4 /EHsc)
target_include_directories(raw PRIVATE include)
target_link_libraries(raw PRIVATE libraw ws2_32 windowscodecs ole32)
set_target_properties(raw PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}/plugins)

set(VCOMP_DLL "$ENV{VCToolsRedistDir}x64/Microsoft.VC143.OpenMP/vcomp140.dll")
if(EXISTS "${VCOMP_DLL}")
    add_custom_command(TARGET raw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VCOMP_DLL}" "${OUT_DIR}/plugins/vcomp140.dll")
else()
    message(WARNING "vcomp140.dll introuvable : le plugin RAW dépendra du redistribuable Visual C++")
endif()

# Textures Unreal Engine (.uasset non cookés et cookés). Oodle est chargé depuis une installation d'Unreal.
add_library(uasset SHARED plugins/uasset/uasset.cpp plugins/uasset/codecs.cpp plugins/uasset/gpu_decode.cpp src/inflate.cpp)
target_compile_options(uasset PRIVATE /utf-8 /W4 /EHsc)
target_include_directories(uasset PRIVATE include)
target_link_libraries(uasset PRIVATE windowscodecs ole32 advapi32 d3d11)
set_target_properties(uasset PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}/plugins)

# --- Installeur ------------------------------------------------------------------------------
# Programme d'installation ; tools/package.ps1 lui colle l'archive de release pour produire
# dist/CyImgView-<version>-Setup.exe.
add_executable(CyImgViewSetup WIN32 installer/setup.cpp installer/setup.rc installer/setup.manifest
    src/updater.cpp src/inflate.cpp)
target_compile_options(CyImgViewSetup PRIVATE ${CYIMG_WARNINGS})
target_link_libraries(CyImgViewSetup PRIVATE comctl32 shell32 ole32 advapi32 winhttp bcrypt)
set_target_properties(CyImgViewSetup PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR} OUTPUT_NAME setup-stub)

# TGA (Truevision) : non géré par WIC
add_library(tga SHARED plugins/tga/tga.cpp)
target_compile_options(tga PRIVATE /utf-8 /W4 /EHsc)
target_include_directories(tga PRIVATE include)
set_target_properties(tga PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}/plugins)
