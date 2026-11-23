add_executable(test_plugin test_plugin.c)
target_include_directories(test_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../third-party/utest)
# hello_plugin.c includes <core/*.h>: pull core INTERFACE paths
target_link_libraries(test_plugin PRIVATE core_headers)
# vendored utest.h does not pass -Werror; tests are not product code
target_compile_options(test_plugin PRIVATE -Wno-everything -fsanitize=address,undefined -g)
target_link_options(test_plugin PRIVATE -fsanitize=address,undefined -Wl,--wrap=malloc)
add_test(NAME test_plugin COMMAND test_plugin)
# the test includes hello_plugin.c: generated asset header
add_dependencies(test_plugin hello_plugin_page_assets)
target_include_directories(test_plugin PRIVATE ${CMAKE_BINARY_DIR}/examples/plugin/lib)

# Built-artifact test: dlopen + dlsym (.so path comes via env)
add_executable(test_load test_load.c)
target_include_directories(test_load PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../third-party/utest)
target_link_libraries(test_load PRIVATE core_headers ${CMAKE_DL_LIBS})
target_compile_options(test_load PRIVATE -Wno-everything -fsanitize=address,undefined -g)
# NO -Wl,--wrap=malloc here: dlopen allocates inside the loader
target_link_options(test_load PRIVATE -fsanitize=address,undefined)
add_test(NAME test_load COMMAND test_load)
set_tests_properties(test_load PROPERTIES
    ENVIRONMENT "PLUGIN_SO=$<TARGET_FILE:hello_plugin>")
