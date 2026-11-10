idf_component_register(
    SRCS "src/api.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_update config dfu32_core dfu_ble esp_app_format esp_http_server esp_timer fetch job mbedtls
                  net runlog staging
)

# The web UI (SPEC.md §10.2): one HTML file, gzipped at build time (same bytes
# every build) and embedded as _binary_index_html_gz_start/_end.
idf_build_get_property(python PYTHON)
set(ui_src "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html")
set(ui_tool "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/dfu32_ui.py")
set(ui_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
# Inlined by the build (/* @dfu32-include ... */): the installer page's DFU package reader.
set(ui_includes "${CMAKE_CURRENT_SOURCE_DIR}/../../web/staging.js")
add_custom_command(
    OUTPUT "${ui_gz}"
    COMMAND "${python}" "${ui_tool}" build "${ui_src}" "${ui_gz}"
    DEPENDS "${ui_src}" "${ui_tool}" ${ui_includes}
    COMMENT "Compressing the web UI"
    VERBATIM)
add_custom_target(dfu32_ui_gz DEPENDS "${ui_gz}")
add_dependencies(${COMPONENT_LIB} dfu32_ui_gz)
target_add_binary_data(${COMPONENT_LIB} "${ui_gz}" BINARY)
