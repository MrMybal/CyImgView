# Formats classiques sans bibliothèque tierce : Netpbm, PCX, XBM/XPM, IFF ILBM/PBM, Radiance HDR,
# FITS, ICNS, PICT.
cyimg_add_plugin(classic
    ${CYIMG_PLUGIN_DIR}/classic.cpp
    ${CYIMG_PLUGIN_DIR}/netpbm.cpp
    ${CYIMG_PLUGIN_DIR}/pcx.cpp
    ${CYIMG_PLUGIN_DIR}/xbitmap.cpp
    ${CYIMG_PLUGIN_DIR}/iff.cpp
    ${CYIMG_PLUGIN_DIR}/hdr.cpp
    ${CYIMG_PLUGIN_DIR}/fits.cpp
    ${CYIMG_PLUGIN_DIR}/icns.cpp
    ${CYIMG_PLUGIN_DIR}/pict.cpp)
