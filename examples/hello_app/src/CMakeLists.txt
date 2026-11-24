# hello_app compiles the hello_server modules directly: the app is the
# server plus policy (login gate, dashboard), no duplicated sources.
set(HELLO_SERVER_SRC ${CMAKE_SOURCE_DIR}/examples/hello_server/src)

add_executable(hello_app
    main.c
    app_logs.c
    login.c
    sessions.c
    ${HELLO_SERVER_SRC}/db.c
    ${HELLO_SERVER_SRC}/collect.c
    ${HELLO_SERVER_SRC}/metrics.c)
target_include_directories(hello_app PRIVATE ${HELLO_SERVER_SRC})
target_link_libraries(hello_app PRIVATE core_headers core_lib core_srv)
target_link_libraries(hello_app PRIVATE sqlite3)

include(embed_asset)
core_embed_asset(hello_app index ${CMAKE_CURRENT_SOURCE_DIR}/../assets/index.html)
core_embed_asset(hello_app login ${CMAKE_CURRENT_SOURCE_DIR}/../assets/login.html)
core_embed_asset(hello_app style ${CMAKE_CURRENT_SOURCE_DIR}/../assets/style.css)
core_embed_asset(hello_app htmx ${CMAKE_SOURCE_DIR}/third-party/htmx/htmx.min.js)
core_embed_asset(hello_app uplot_js ${CMAKE_SOURCE_DIR}/third-party/uplot/uPlot.iife.min.js)
core_embed_asset(hello_app uplot_css ${CMAKE_SOURCE_DIR}/third-party/uplot/uPlot.min.css)
