add_executable(hello_server main.c)
target_link_libraries(hello_server PRIVATE core_headers core_lib core_srv)
target_link_libraries(hello_server PRIVATE ${CMAKE_DL_LIBS})
# Plugin path is baked in at build time
target_compile_definitions(hello_server PRIVATE PLUGIN_PATH="$<TARGET_FILE:hello_plugin>")
add_dependencies(hello_server hello_plugin)
include(embed_asset)
core_embed_asset(hello_server index ${CMAKE_CURRENT_SOURCE_DIR}/../assets/index.html)
core_embed_asset(hello_server htmx ${CMAKE_SOURCE_DIR}/third-party/htmx/htmx.min.js)
