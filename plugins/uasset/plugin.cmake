# Textures Unreal Engine (.uasset non cookés et cookés). Oodle est chargé depuis une installation d'Unreal.
cyimg_add_plugin(uasset ${CYIMG_PLUGIN_DIR}/uasset.cpp ${CYIMG_PLUGIN_DIR}/codecs.cpp)
target_link_libraries(uasset PRIVATE windowscodecs ole32 advapi32)
