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

# --- Code partagé (application, installeur, plugins) -----------------------------------------
# deflate/zlib, archives zip, décodage des formats GPU par blocs (BC1 à BC7)
add_library(cyimg_support STATIC src/inflate.cpp src/zip.cpp src/gpu_decode.cpp src/i18n.cpp)
target_compile_options(cyimg_support PRIVATE ${CYIMG_WARNINGS})
target_include_directories(cyimg_support PUBLIC src include)
target_link_libraries(cyimg_support PUBLIC d3d11)

# --- Langues ---------------------------------------------------------------------------------
# Tous les fichiers src/lang/*.lang sont intégrés comme ressources « LANG » (voir src/i18n.h) :
# gen/lang.rc est régénéré à chaque configuration, et les .rc sont recompilés quand un .lang change.
file(GLOB CYIMG_LANG_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/lang/*.lang)
set(CYIMG_LANG_RC "")
set(lang_id 1)
foreach(f ${CYIMG_LANG_FILES})
    file(TO_NATIVE_PATH "${f}" native)
    string(REPLACE "\\" "\\\\" native "${native}")
    string(APPEND CYIMG_LANG_RC "${lang_id} LANG \"${native}\"
")
    math(EXPR lang_id "${lang_id} + 1")
endforeach()
file(WRITE ${CMAKE_BINARY_DIR}/gen/lang.rc.tmp "${CYIMG_LANG_RC}")
file(COPY_FILE ${CMAKE_BINARY_DIR}/gen/lang.rc.tmp ${CMAKE_BINARY_DIR}/gen/lang.rc ONLY_IF_DIFFERENT)
set_source_files_properties(src/app.rc installer/setup.rc tools/bench.rc PROPERTIES
    OBJECT_DEPENDS "${CYIMG_LANG_FILES};${CMAKE_BINARY_DIR}/gen/lang.rc")

# --- Application -----------------------------------------------------------------------------
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
    src/viewer_extra.cpp
    src/viewer_slideshow.cpp
    src/updater.cpp
    src/app.rc
    src/app.manifest)
target_compile_options(CyImgView PRIVATE ${CYIMG_WARNINGS})
target_link_libraries(CyImgView PRIVATE cyimg_support d2d1 d3d11 dxgi dwrite windowscodecs shlwapi dwmapi shell32
    ole32 propsys comctl32 comdlg32 winhttp bcrypt dxguid)
target_include_directories(CyImgView PRIVATE ${CMAKE_BINARY_DIR}/gen)
set_target_properties(CyImgView PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})

# Banc d'essai console du décodage (mesure des temps, conversions, tests des mises à jour)
add_executable(cyimg_bench tools/bench.cpp tools/bench.rc src/decoder.cpp src/folder.cpp src/save.cpp src/updater.cpp)
target_compile_options(cyimg_bench PRIVATE ${CYIMG_WARNINGS})
target_link_libraries(cyimg_bench PRIVATE cyimg_support windowscodecs shlwapi ole32 propsys winhttp bcrypt)
target_include_directories(cyimg_bench PRIVATE ${CMAKE_BINARY_DIR}/gen)
set_target_properties(cyimg_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})

# --- Installeur ------------------------------------------------------------------------------
# Programme d'installation ; tools/package.ps1 lui colle l'archive de release pour produire
# dist/CyImgView-<version>-Setup.exe.
add_executable(CyImgViewSetup WIN32 installer/setup.cpp installer/setup.rc installer/setup.manifest src/updater.cpp)
target_compile_options(CyImgViewSetup PRIVATE ${CYIMG_WARNINGS})
target_link_libraries(CyImgViewSetup PRIVATE cyimg_support comctl32 shell32 ole32 advapi32 winhttp bcrypt)
target_include_directories(CyImgViewSetup PRIVATE ${CMAKE_BINARY_DIR}/gen)
set_target_properties(CyImgViewSetup PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR} OUTPUT_NAME setup-stub)

# --- Plugins ---------------------------------------------------------------------------------
# cyimg_add_plugin(nom sources...) : DLL dans bin/plugins, liée au code partagé.
function(cyimg_add_plugin name)
    add_library(${name} SHARED ${ARGN})
    target_compile_options(${name} PRIVATE /utf-8 /W4 /EHsc)
    target_link_libraries(${name} PRIVATE cyimg_support)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}/plugins)
endfunction()

# Chaque dossier plugins/<nom>/ décrit sa compilation dans un plugin.cmake.
# -DCYIMG_ONLY_PLUGINS="a;b" : ne configure que ces plugins (développement, builds parallèles).
set(CYIMG_ONLY_PLUGINS "" CACHE STRING "Liste de plugins à configurer (vide = tous)")
file(GLOB CYIMG_PLUGIN_SCRIPTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/plugins/*/plugin.cmake)
foreach(script ${CYIMG_PLUGIN_SCRIPTS})
    get_filename_component(CYIMG_PLUGIN_DIR ${script} DIRECTORY)
    get_filename_component(CYIMG_PLUGIN_NAME ${CYIMG_PLUGIN_DIR} NAME)
    if(CYIMG_ONLY_PLUGINS AND NOT CYIMG_PLUGIN_NAME IN_LIST CYIMG_ONLY_PLUGINS)
        continue()
    endif()
    include(${script})
endforeach()
