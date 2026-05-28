cmake_minimum_required(VERSION 3.16)
project(FastClone LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(FastClone
    FastClone/cli.cpp
    FastClone/file_index.cpp
    FastClone/hash_memcache.cpp
    FastClone/main.cpp
    FastClone/protocol.cpp
    FastClone/sync_engine.cpp
    FastClone/win_socket.cpp
)

target_include_directories(FastClone PRIVATE FastClone)

find_package(Threads REQUIRED)
target_link_libraries(FastClone PRIVATE Threads::Threads)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(XXHASH QUIET libxxhash xxhash)
endif()

if(XXHASH_FOUND)
    target_include_directories(FastClone PRIVATE ${XXHASH_INCLUDE_DIRS})
    target_link_directories(FastClone PRIVATE ${XXHASH_LIBRARY_DIRS})
    target_link_libraries(FastClone PRIVATE ${XXHASH_LIBRARIES})
    target_compile_options(FastClone PRIVATE ${XXHASH_CFLAGS_OTHER})
else()
    find_library(XXHASH_LIBRARY NAMES xxhash libxxhash)
    if(NOT XXHASH_LIBRARY)
        message(FATAL_ERROR "xxhash library not found. Install libxxhash development package.")
    endif()
    find_path(XXHASH_INCLUDE_DIR NAMES xxhash.h)
    if(NOT XXHASH_INCLUDE_DIR)
        message(FATAL_ERROR "xxhash headers not found. Install libxxhash development package.")
    endif()
    target_include_directories(FastClone PRIVATE ${XXHASH_INCLUDE_DIR})
    target_link_libraries(FastClone PRIVATE ${XXHASH_LIBRARY})
endif()

if(WIN32)
    target_compile_definitions(FastClone PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(FastClone PRIVATE ws2_32)
endif()

enable_testing()

add_executable(FastCloneTests
    FastClone/cli.cpp
    FastClone/hash_memcache.cpp
    tests/test_main.cpp
    tests/test_cli.cpp
    tests/test_hash_memcache.cpp
)
target_include_directories(FastCloneTests PRIVATE FastClone)
target_link_libraries(FastCloneTests PRIVATE Threads::Threads)
if(WIN32)
    target_compile_definitions(FastCloneTests PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
add_test(NAME FastCloneTests COMMAND FastCloneTests)

install(TARGETS FastClone
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
