# Documents graphiques : Krita, OpenRaster, GIMP, Clip Studio Paint, EPS, InDesign, CorelDRAW, Affinity
# (composite ou aperçu intégré, sans bibliothèque tierce)
cyimg_add_plugin(docs ${CYIMG_PLUGIN_DIR}/docs.cpp ${CYIMG_PLUGIN_DIR}/xcf.cpp ${CYIMG_PLUGIN_DIR}/clip.cpp
                 ${CYIMG_PLUGIN_DIR}/eps.cpp)
target_link_libraries(docs PRIVATE gdi32)
