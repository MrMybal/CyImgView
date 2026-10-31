cyimg_add_plugin(hlsl ${CYIMG_PLUGIN_DIR}/hlsl.cpp)
target_link_libraries(hlsl PRIVATE d3d11 d3dcompiler dxgi)
add_custom_command(TARGET hlsl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hlsl>/effects"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CYIMG_PLUGIN_DIR}/warmth.hlsl" "$<TARGET_FILE_DIR:hlsl>/effects/warmth.hlsl")
