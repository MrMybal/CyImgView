# Actions « Ouvrir avec » (plugin d'édition d'exemple : imprimer, ouvrir dans le navigateur)
cyimg_add_plugin(share_actions ${CYIMG_PLUGIN_DIR}/share_actions.cpp)
target_link_libraries(share_actions PRIVATE shlwapi)
