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

set(VCOMP_DLL "$ENV{VCToolsRedistDir}x64/Microsoft.VC143.OpenMP/vcomp140.dll")
if(EXISTS "${VCOMP_DLL}")
    add_custom_command(TARGET raw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VCOMP_DLL}" "${OUT_DIR}/plugins/vcomp140.dll")
else()
    message(WARNING "vcomp140.dll introuvable : le plugin RAW dépendra du redistribuable Visual C++")
endif()
