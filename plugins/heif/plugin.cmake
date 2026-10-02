# HEIF / HEIC / AVIF : libheif (LGPL 3) + libde265 (HEVC, LGPL 3) + dav1d (AV1, BSD 2), en statique.
# Les CMakeLists d'origine ne sont pas utilisés (options globales, find_package...) : les sources sont
# listées ici et les en-têtes de configuration normalement générés sont fournis dans gen/.
set(HEIF_TP ${CMAKE_SOURCE_DIR}/third_party)
set(HEIF_GEN ${CYIMG_PLUGIN_DIR}/gen)

# --- dav1d 1.5.4 (AV1) ----------------------------------------------------------------------
# nasm absent : compilé sans l'assembleur x86 (HAVE_ASM 0 dans gen/dav1d/config.h).
set(DAV1D_DIR ${HEIF_TP}/dav1d)
set(DAV1D_INCLUDES ${HEIF_GEN}/dav1d ${DAV1D_DIR} ${DAV1D_DIR}/include/dav1d ${DAV1D_DIR}/include
    ${DAV1D_DIR}/include/compat/msvc)
set(DAV1D_SOURCES cdf.c cpu.c ctx.c data.c decode.c dequant_tables.c getbits.c intra_edge.c itx_1d.c
    lf_mask.c lib.c log.c mem.c msac.c obu.c pal.c picture.c qm.c ref.c refmvs.c scan.c tables.c
    thread_task.c warpmv.c wedge.c win32/thread.c)
# Gabarits compilés une fois par profondeur (8 et 16 bits) avec -DBITDEPTH=...
set(DAV1D_TMPL_SOURCES cdef_apply_tmpl.c cdef_tmpl.c fg_apply_tmpl.c filmgrain_tmpl.c ipred_prepare_tmpl.c
    ipred_tmpl.c itx_tmpl.c lf_apply_tmpl.c loopfilter_tmpl.c looprestoration_tmpl.c lr_apply_tmpl.c
    mc_tmpl.c recon_tmpl.c)
list(TRANSFORM DAV1D_SOURCES PREPEND ${DAV1D_DIR}/src/)
list(TRANSFORM DAV1D_TMPL_SOURCES PREPEND ${DAV1D_DIR}/src/)
set(DAV1D_OPTIONS /W0 /O2 /wd4028 /wd4090 /wd4996)

foreach(bd 8 16)
    add_library(cyimg_dav1d_bd${bd} OBJECT ${DAV1D_TMPL_SOURCES})
    target_include_directories(cyimg_dav1d_bd${bd} PRIVATE ${DAV1D_INCLUDES})
    target_compile_definitions(cyimg_dav1d_bd${bd} PRIVATE BITDEPTH=${bd})
    target_compile_options(cyimg_dav1d_bd${bd} PRIVATE ${DAV1D_OPTIONS})
    set_target_properties(cyimg_dav1d_bd${bd} PROPERTIES C_STANDARD 11)
endforeach()

add_library(cyimg_dav1d STATIC ${DAV1D_SOURCES}
    $<TARGET_OBJECTS:cyimg_dav1d_bd8> $<TARGET_OBJECTS:cyimg_dav1d_bd16>)
target_include_directories(cyimg_dav1d PRIVATE ${DAV1D_INCLUDES} PUBLIC ${DAV1D_DIR}/include)
target_compile_options(cyimg_dav1d PRIVATE ${DAV1D_OPTIONS})
set_target_properties(cyimg_dav1d PROPERTIES C_STANDARD 11)

# --- libde265 1.1.3 (HEVC) ------------------------------------------------------------------
set(DE265_DIR ${HEIF_TP}/libde265)
set(DE265_SOURCES alloc_pool.cc bitstream.cc cabac.cc contextmodel.cc de265.cc deblock.cc decctx.cc dpb.cc
    fallback-dct.cc fallback-deblk.cc fallback-intrapred.cc fallback-motion.cc fallback.cc image-io.cc
    image.cc intrapred.cc md5.cc motion.cc nal-parser.cc nal.cc pps.cc quality.cc refpic.cc sao.cc scan.cc
    sei.cc slice.cc sps.cc threads.cc transform.cc util.cc visualize.cc vps.cc vui.cc
    x86/sse.cc x86/sse-motion.cc x86/sse-dct.cc x86/sse-intrapred.cc x86/sse-deblk.cc)
list(TRANSFORM DE265_SOURCES PREPEND ${DE265_DIR}/libde265/)
add_library(cyimg_libde265 STATIC ${DE265_SOURCES} ${DE265_DIR}/extra/win32cond.c)
target_include_directories(cyimg_libde265
    PRIVATE ${HEIF_GEN}/de265 ${DE265_DIR}/libde265 ${DE265_DIR}/extra
    PUBLIC ${DE265_DIR} ${HEIF_GEN}/de265)
target_compile_definitions(cyimg_libde265 PUBLIC LIBDE265_STATIC_BUILD PRIVATE HAVE_CONFIG_H _CRT_SECURE_NO_WARNINGS)
target_compile_options(cyimg_libde265 PRIVATE /W0 /EHsc)

# --- libheif 1.23.5 -------------------------------------------------------------------------
# Lecture seule : décodeurs intégrés libde265 + dav1d (+ codec « non compressé », sans dépendance).
# Les fichiers *_enc.cc / encoder_mask.cc font partie du noyau (référencés par l'API) : aucun
# encodeur externe n'est compilé.
set(HEIF_DIR ${HEIF_TP}/libheif/libheif)
file(GLOB HEIF_SOURCES CONFIGURE_DEPENDS
    ${HEIF_DIR}/*.cc ${HEIF_DIR}/api/libheif/*.cc ${HEIF_DIR}/codecs/*.cc ${HEIF_DIR}/codecs/uncompressed/*.cc
    ${HEIF_DIR}/color-conversion/*.cc ${HEIF_DIR}/image/*.cc ${HEIF_DIR}/image-items/*.cc
    ${HEIF_DIR}/sequences/*.cc)
list(FILTER HEIF_SOURCES EXCLUDE REGEX "/plugins_(unix|windows)\\.cc$|/heif_experimental\\.cc$")
list(APPEND HEIF_SOURCES
    ${HEIF_DIR}/plugins/decoder_libde265.cc ${HEIF_DIR}/plugins/decoder_dav1d.cc
    ${HEIF_DIR}/plugins/decoder_uncompressed.cc ${HEIF_DIR}/plugins/encoder_uncompressed.cc
    ${HEIF_DIR}/plugins/encoder_mask.cc ${HEIF_DIR}/plugins/nalu_utils.cc)
add_library(cyimg_libheif STATIC ${HEIF_SOURCES})
target_include_directories(cyimg_libheif
    PRIVATE ${HEIF_DIR}
    PUBLIC ${HEIF_DIR}/api ${HEIF_GEN}/libheif)
target_compile_definitions(cyimg_libheif
    PUBLIC LIBHEIF_STATIC_BUILD
    PRIVATE HAVE_LIBDE265=1 HAVE_DAV1D=1 WITH_UNCOMPRESSED_CODEC=1 HAVE_BIT IS_BIG_ENDIAN=0
            ENABLE_MULTITHREADING_SUPPORT=1 ENABLE_PARALLEL_TILE_DECODING=1 _CRT_SECURE_NO_WARNINGS)
target_compile_options(cyimg_libheif PRIVATE /W0 /EHsc /bigobj)
target_link_libraries(cyimg_libheif PUBLIC cyimg_libde265 cyimg_dav1d)

# --- Plugin ---------------------------------------------------------------------------------
cyimg_add_plugin(heif ${CYIMG_PLUGIN_DIR}/heif.cpp)
target_link_libraries(heif PRIVATE cyimg_libheif)
