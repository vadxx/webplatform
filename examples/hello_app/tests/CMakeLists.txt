# Vendored utest.h lives in third-party/utest
set(UTEST_INC ${CMAKE_SOURCE_DIR}/third-party/utest)

# hello_app compiles the hello_server modules directly; tests reach them
# through ../src includes (same pattern as hello_server/tests)
set(HELLO_SERVER_SRC ${CMAKE_SOURCE_DIR}/examples/hello_server/src)

add_executable(test_sessions_store test_sessions_store.c)
target_include_directories(test_sessions_store PRIVATE ${UTEST_INC} ${HELLO_SERVER_SRC})
target_link_libraries(test_sessions_store PRIVATE sqlite3 core_headers core_lib core_srv)
# vendored code does not pass -Werror; tests are not product code
target_compile_options(test_sessions_store PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_sessions_store PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_sessions_store COMMAND test_sessions_store)

add_executable(test_login test_login.c)
target_include_directories(test_login PRIVATE ${UTEST_INC} ${HELLO_SERVER_SRC})
target_link_libraries(test_login PRIVATE sqlite3 core_headers core_lib core_srv)
# vendored code does not pass -Werror; tests are not product code
target_compile_options(test_login PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_login PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_login COMMAND test_login)

add_executable(test_app_logs test_app_logs.c)
target_include_directories(test_app_logs PRIVATE ${UTEST_INC} ${HELLO_SERVER_SRC})
target_link_libraries(test_app_logs PRIVATE sqlite3 core_headers core_lib core_srv)
# vendored code does not pass -Werror; tests are not product code
target_compile_options(test_app_logs PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_app_logs PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_app_logs COMMAND test_app_logs)

