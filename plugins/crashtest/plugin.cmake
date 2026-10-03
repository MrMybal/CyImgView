# Plugin de test volontairement défaillant (robustesse de l'hôte) : seulement avec
# -DCYIMG_TEST_PLUGINS=ON, compilé dans bin/testplugins (jamais dans bin/plugins, donc jamais livré).
if(CYIMG_TEST_PLUGINS)
    cyimg_add_plugin(crashtest ${CYIMG_PLUGIN_DIR}/crashtest.cpp)
    set_target_properties(crashtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}/testplugins)
endif()
