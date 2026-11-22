cmake_minimum_required(VERSION 3.20)
project(webplatform_demo LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Common hard embedded flags
add_compile_options(
    -Wall -Wextra -Wpedantic -Werror
    -fvisibility=hidden
    -ffunction-sections -fdata-sections
)
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
)

enable_testing()

# Platform libraries (core is unconditional; siblings appear in the server repo)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/core/cmake)
add_subdirectory(lib)

option(WEBPLATFORM_BUILD_EXAMPLES "Build demo examples" ON)
if(WEBPLATFORM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
