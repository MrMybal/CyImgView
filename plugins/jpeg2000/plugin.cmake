# JPEG 2000 (JP2/J2K/JPX/HTJ2K) avec OpenJPEG (third_party/openjpeg, BSD-2)
set(OPJ_DIR ${CMAKE_SOURCE_DIR}/third_party/openjpeg/src/lib/openjp2)
add_library(cyimg_openjp2 STATIC
    ${OPJ_DIR}/thread.c ${OPJ_DIR}/bio.c ${OPJ_DIR}/cio.c ${OPJ_DIR}/dwt.c ${OPJ_DIR}/event.c
    ${OPJ_DIR}/ht_dec.c ${OPJ_DIR}/image.c ${OPJ_DIR}/invert.c ${OPJ_DIR}/j2k.c ${OPJ_DIR}/jp2.c
    ${OPJ_DIR}/mct.c ${OPJ_DIR}/mqc.c ${OPJ_DIR}/openjpeg.c ${OPJ_DIR}/opj_clock.c ${OPJ_DIR}/pi.c
    ${OPJ_DIR}/t1.c ${OPJ_DIR}/t2.c ${OPJ_DIR}/tcd.c ${OPJ_DIR}/tgt.c ${OPJ_DIR}/function_list.c
    ${OPJ_DIR}/opj_malloc.c ${OPJ_DIR}/sparse_array.c)
# opj_config.h / opj_config_private.h écrits à la main dans plugins/jpeg2000/config
target_include_directories(cyimg_openjp2 PUBLIC ${OPJ_DIR} ${CYIMG_PLUGIN_DIR}/config)
target_compile_definitions(cyimg_openjp2 PUBLIC OPJ_STATIC PRIVATE MUTEX_win32 _CRT_SECURE_NO_WARNINGS)
target_compile_options(cyimg_openjp2 PRIVATE /W0)

cyimg_add_plugin(jpeg2000 ${CYIMG_PLUGIN_DIR}/jpeg2000.cpp)
target_link_libraries(jpeg2000 PRIVATE cyimg_openjp2)
