add_library(core_headers INTERFACE)
target_include_directories(core_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(core_lib OBJECT src/uri.c)
target_link_libraries(core_lib PUBLIC core_headers)
set_target_properties(core_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
# -nostdlib .so must not pull in __stack_chk_fail
target_compile_options(core_lib PRIVATE -fno-stack-protector)

# Server part: epoll/sockets/http/log/sessions, executables only, never a -nostdlib .so
add_library(core_srv OBJECT src/event_loop.c src/http.c src/log.c src/session.c)
set_target_properties(core_srv PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(core_srv PUBLIC core_headers)

add_subdirectory(tests)
