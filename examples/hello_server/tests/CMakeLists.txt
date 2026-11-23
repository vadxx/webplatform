# Vendored utest.h lives in third-party/utest
set(UTEST_INC ${CMAKE_SOURCE_DIR}/third-party/utest)

add_executable(test_db test_db.c)
target_include_directories(test_db PRIVATE ${UTEST_INC})
target_link_libraries(test_db PRIVATE sqlite3)
# vendored code does not pass -Werror; tests are not product code
target_compile_options(test_db PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_db PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_db COMMAND test_db)

add_executable(test_collect test_collect.c)
target_include_directories(test_collect PRIVATE ${UTEST_INC})
target_link_libraries(test_collect PRIVATE sqlite3 core_headers core_lib core_srv)
# vendored code does not pass -Werror; tests are not product code
target_compile_options(test_collect PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_collect PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_collect COMMAND test_collect)

add_executable(test_metrics test_metrics.c)
target_include_directories(test_metrics PRIVATE ${UTEST_INC})
target_link_libraries(test_metrics PRIVATE sqlite3 core_headers core_lib core_srv)
# vendored code does not pass -Werror; tests are not product code
target_compile_options(test_metrics PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_metrics PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_metrics COMMAND test_metrics)
