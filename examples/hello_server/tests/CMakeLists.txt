# Vendored utest.h lives in third-party/utest
set(UTEST_INC ${CMAKE_SOURCE_DIR}/third-party/utest)

add_executable(test_db test_db.c)
target_include_directories(test_db PRIVATE ${UTEST_INC})
target_link_libraries(test_db PRIVATE sqlite3)
# vendored code does not pass -Werror; tests are not product code
target_compile_options(test_db PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_db PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_db COMMAND test_db)
