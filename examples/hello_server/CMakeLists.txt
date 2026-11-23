# Vendored SQLite (see third-party/sqlite/README.md): definitions only,
# never edits; linked into the server executable.
add_library(sqlite3 STATIC ${CMAKE_SOURCE_DIR}/third-party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_SOURCE_DIR}/third-party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DQS=0
    SQLITE_ENABLE_MEMSYS5
)

add_subdirectory(src)
add_subdirectory(tests)
