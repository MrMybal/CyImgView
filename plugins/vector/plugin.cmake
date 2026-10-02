# Formats vectoriels (SVG, PDF/AI, EMF/WMF, DXF) rendus avec Direct2D, DirectWrite et Windows.Data.Pdf
cyimg_add_plugin(vector ${CYIMG_PLUGIN_DIR}/vector.cpp ${CYIMG_PLUGIN_DIR}/svg.cpp ${CYIMG_PLUGIN_DIR}/metafile.cpp
    ${CYIMG_PLUGIN_DIR}/pdf.cpp ${CYIMG_PLUGIN_DIR}/dxf.cpp)
target_link_libraries(vector PRIVATE d2d1 dwrite d3d11 dxgi windowscodecs shlwapi shcore gdi32 ole32 windowsapp windows.data.pdf)
