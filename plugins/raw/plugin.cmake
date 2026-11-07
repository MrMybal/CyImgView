# RAW : aperçus embarqués + LibRaw (third_party/LibRaw, LGPL 2.1 / CDDL 1.0)
set(LIBRAW_DIR ${CMAKE_SOURCE_DIR}/third_party/LibRaw)
file(GLOB_RECURSE LIBRAW_SOURCES CONFIGURE_DEPENDS ${LIBRAW_DIR}/src/*.cpp)
list(FILTER LIBRAW_SOURCES EXCLUDE REGEX "/src/integration/|_ph\.cpp$")  # *_ph.cpp : agrégats (#include) des autres sources
add_library(libraw STATIC ${LIBRAW_SOURCES})
target_include_directories(libraw PUBLIC ${LIBRAW_DIR})
target_compile_definitions(libraw PUBLIC LIBRAW_NODLL PRIVATE LIBRAW_BUILDLIB _CRT_SECURE_NO_WARNINGS)
# OpenMP : dématriçage et décodage multithreads (nécessite vcomp140.dll, copiée à côté du plugin)
target_compile_options(libraw PRIVATE /W0 /EHsc /openmp /bigobj)
set_target_properties(libraw PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

cyimg_add_plugin(raw ${CYIMG_PLUGIN_DIR}/raw.cpp ${CYIMG_PLUGIN_DIR}/previews.cpp)
target_link_libraries(raw PRIVATE libraw ws2_32 windowscodecs ole32)

# Le nom du dossier change avec la génération de MSVC (VC143, VC145, etc.).
# Le runtime doit venir du redistribuable de l'outil actif, jamais du système hôte.
file(GLOB VCOMP_DLL_CANDIDATES "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.OpenMP/vcomp140.dll")
list(SORT VCOMP_DLL_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
if(NOT VCOMP_DLL_CANDIDATES)
    message(FATAL_ERROR "vcomp140.dll introuvable dans VCToolsRedistDir : installer le redistribuable MSVC OpenMP x64 avant de distribuer CyImgView.")
endif()
list(GET VCOMP_DLL_CANDIDATES 0 VCOMP_DLL)
message(STATUS "RAW OpenMP runtime: ${VCOMP_DLL}")
add_custom_command(TARGET raw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VCOMP_DLL}" "${OUT_DIR}/plugins/vcomp140.dll")
