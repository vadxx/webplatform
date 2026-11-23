add_executable(plugin_host main.cpp)
target_link_libraries(plugin_host PRIVATE core_headers core_lib core_srv)
target_link_libraries(plugin_host PRIVATE ${CMAKE_DL_LIBS})
