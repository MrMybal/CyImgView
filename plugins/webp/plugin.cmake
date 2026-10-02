# WebP (fixe et animé) : libwebp (third_party/libwebp, BSD 3 clauses), décodeur + demux seulement
set(LIBWEBP_DIR ${CMAKE_SOURCE_DIR}/third_party/libwebp)
set(LIBWEBP_SOURCES
    # décodeur
    src/dec/alpha_dec.c src/dec/buffer_dec.c src/dec/frame_dec.c src/dec/idec_dec.c src/dec/io_dec.c
    src/dec/quant_dec.c src/dec/tree_dec.c src/dec/vp8_dec.c src/dec/vp8l_dec.c src/dec/webp_dec.c
    # demux + animations
    src/demux/demux.c src/demux/anim_decode.c
    # dsp (C + SSE2/SSE4.1/AVX2, choisis à l'exécution selon le processeur)
    src/dsp/alpha_processing.c src/dsp/cpu.c src/dsp/dec.c src/dsp/dec_clip_tables.c src/dsp/filters.c
    src/dsp/lossless.c src/dsp/rescaler.c src/dsp/upsampling.c src/dsp/yuv.c
    src/dsp/alpha_processing_sse2.c src/dsp/dec_sse2.c src/dsp/filters_sse2.c src/dsp/lossless_sse2.c
    src/dsp/rescaler_sse2.c src/dsp/upsampling_sse2.c src/dsp/yuv_sse2.c
    src/dsp/alpha_processing_sse41.c src/dsp/dec_sse41.c src/dsp/lossless_sse41.c src/dsp/upsampling_sse41.c
    src/dsp/yuv_sse41.c src/dsp/lossless_avx2.c
    # utilitaires
    src/utils/bit_reader_utils.c src/utils/color_cache_utils.c src/utils/filters_utils.c src/utils/huffman_utils.c
    src/utils/palette.c src/utils/quant_levels_dec_utils.c src/utils/random_utils.c src/utils/rescaler_utils.c
    src/utils/thread_utils.c src/utils/utils.c)
list(TRANSFORM LIBWEBP_SOURCES PREPEND ${LIBWEBP_DIR}/)
add_library(cyimg_libwebp STATIC ${LIBWEBP_SOURCES})
target_include_directories(cyimg_libwebp PUBLIC ${LIBWEBP_DIR} ${LIBWEBP_DIR}/src)
target_compile_definitions(cyimg_libwebp PRIVATE WEBP_USE_THREAD _CRT_SECURE_NO_WARNINGS)
target_compile_options(cyimg_libwebp PRIVATE /W0)

cyimg_add_plugin(webp ${CYIMG_PLUGIN_DIR}/webp.cpp)
target_link_libraries(webp PRIVATE cyimg_libwebp)
