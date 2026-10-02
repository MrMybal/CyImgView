# Photoshop PSD/PSB : image composite fusionnée (sans bibliothèque tierce)
cyimg_add_plugin(psd ${CYIMG_PLUGIN_DIR}/psd.cpp)
target_link_libraries(psd PRIVATE windowscodecs ole32)
