# Vendored utest.h lives in third-party/utest
set(UTEST_INC ${CMAKE_SOURCE_DIR}/third-party/utest)
# Vendored jsmn.h lives in third-party/jsmn
set(JSMN_INC ${CMAKE_SOURCE_DIR}/third-party/jsmn)

add_executable(test_uri test_uri.c)
target_include_directories(test_uri PRIVATE ${UTEST_INC})
target_link_libraries(test_uri PRIVATE core_headers)
# vendored utest.h does not pass -Werror; tests are not product code
target_compile_options(test_uri PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_uri PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_uri COMMAND test_uri)

add_executable(test_event_loop test_event_loop.c)
target_include_directories(test_event_loop PRIVATE ${UTEST_INC})
target_link_libraries(test_event_loop PRIVATE core_headers)
target_compile_options(test_event_loop PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_event_loop PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_event_loop COMMAND test_event_loop)

add_executable(test_http test_http.c)
target_include_directories(test_http PRIVATE ${UTEST_INC})
target_link_libraries(test_http PRIVATE core_headers core_lib ${CMAKE_DL_LIBS})
target_compile_options(test_http PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_http PRIVATE -fsanitize=address,undefined)
add_test(NAME test_http COMMAND test_http)

add_executable(test_jsmn test_jsmn.c)
target_include_directories(test_jsmn PRIVATE ${UTEST_INC} ${JSMN_INC})
target_compile_options(test_jsmn PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_jsmn PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_jsmn COMMAND test_jsmn)
