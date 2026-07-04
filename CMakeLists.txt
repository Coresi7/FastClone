cmake_minimum_required(VERSION 3.16)
project(FastClone LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unified disk IO driver sources (unified-disk-io-driver). The platform backend .cpp files are
# each guarded by their platform macro so exactly one compiles to real code per OS; the others
# compile to an empty translation unit (design D-04, strong isolation).
set(FASTCLONE_DISK_IO_SOURCES
    FastClone/disk_io_align.cpp
    FastClone/disk_io_driver.cpp
    FastClone/disk_io_backend_win.cpp
    FastClone/disk_io_backend_posix.cpp
    FastClone/disk_io_backend_uring.cpp
)

add_executable(FastClone
    FastClone/cli.cpp
    FastClone/client_handshake.cpp
    FastClone/delta.cpp
    FastClone/file_index.cpp
    FastClone/hash_memcache.cpp
    FastClone/main.cpp
    FastClone/net_topology.cpp
    FastClone/protocol.cpp
    FastClone/protocol_codec.cpp
    FastClone/sync_engine_shared.cpp
    FastClone/sync_engine_server.cpp
    FastClone/sync_engine_client.cpp
    FastClone/sync_util.cpp
    FastClone/transfer_tuning.cpp
    FastClone/win_socket.cpp
    ${FASTCLONE_DISK_IO_SOURCES}
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
    target_link_libraries(FastClone PRIVATE ws2_32 iphlpapi bcrypt)
endif()

enable_testing()

add_executable(FastCloneTests
    FastClone/cli.cpp
    FastClone/delta.cpp
    FastClone/file_index.cpp
    FastClone/hash_memcache.cpp
    FastClone/net_topology.cpp
    FastClone/protocol.cpp
    FastClone/protocol_codec.cpp
    FastClone/sync_util.cpp
    FastClone/transfer_tuning.cpp
    FastClone/win_socket.cpp
    ${FASTCLONE_DISK_IO_SOURCES}
    tests/test_main.cpp
    tests/test_cli.cpp
    tests/test_delta.cpp
    tests/test_streaming_buildplan.cpp
    tests/test_disk_io_align.cpp
    tests/test_disk_io_driver.cpp
    tests/test_read_gate.cpp
    tests/test_file_index.cpp
    tests/test_hash_memcache.cpp
    tests/test_reconnect.cpp
    tests/test_route_selection.cpp
    tests/test_link_scheduler.cpp
    tests/test_wan_tuning.cpp
)
target_include_directories(FastCloneTests PRIVATE FastClone)
target_link_libraries(FastCloneTests PRIVATE Threads::Threads)
# CMake build includes test_cli.cpp (RunCliTests is defined); enable its call in test_main.cpp.
# The VS FastCloneTests.vcxproj excludes test_cli.cpp and leaves this macro undefined.
target_compile_definitions(FastCloneTests PRIVATE FASTCLONE_ENABLE_CLI_TESTS)

# file_index.cpp pulls in xxhash; reuse the same resolution as the main target.
if(XXHASH_FOUND)
    target_include_directories(FastCloneTests PRIVATE ${XXHASH_INCLUDE_DIRS})
    target_link_directories(FastCloneTests PRIVATE ${XXHASH_LIBRARY_DIRS})
    target_link_libraries(FastCloneTests PRIVATE ${XXHASH_LIBRARIES})
    target_compile_options(FastCloneTests PRIVATE ${XXHASH_CFLAGS_OTHER})
else()
    target_include_directories(FastCloneTests PRIVATE ${XXHASH_INCLUDE_DIR})
    target_link_libraries(FastCloneTests PRIVATE ${XXHASH_LIBRARY})
endif()

if(WIN32)
    target_compile_definitions(FastCloneTests PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
    # net_topology.cpp (address enumeration / RNG) and win_socket.cpp pull in Winsock,
    # IP Helper and CNG; link them explicitly for the test target (design §8.4 / R-05).
    target_link_libraries(FastCloneTests PRIVATE ws2_32 iphlpapi bcrypt)
endif()
add_test(NAME FastCloneTests COMMAND FastCloneTests)

# Optional io_uring (Linux only). When liburing is present the io_uring backend compiles and links
# against it; when absent, disk_io_backend_uring.cpp's __has_include guard leaves only the bounded
# pread/pwrite pool fallback (design §7.3). macOS/Windows never look for liburing.
if(UNIX AND NOT APPLE)
    find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)
    find_library(LIBURING_LIBRARY NAMES uring)
    if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
        foreach(tgt FastClone FastCloneTests)
            target_include_directories(${tgt} PRIVATE ${LIBURING_INCLUDE_DIR})
            target_link_libraries(${tgt} PRIVATE ${LIBURING_LIBRARY})
        endforeach()
        message(STATUS "FastClone: io_uring backend enabled (liburing found)")
    else()
        message(STATUS "FastClone: liburing not found; Linux disk IO uses the pread/pwrite pool")
    endif()
endif()

install(TARGETS FastClone
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# --- Diagnostics-only multipath route probe (additive; does not touch FastClone/Tests) ---
# The tool reuses the shared client handshake primitives (client_handshake.cpp) and links
# only the self-contained .cpp it needs. It deliberately does NOT link sync_engine.cpp /
# file_index.cpp / xxhash (no transfer engine, no hashing).
add_executable(FastCloneRouteProbe
    FastClone/route_probe_main.cpp
    FastClone/client_handshake.cpp
    FastClone/net_topology.cpp
    FastClone/protocol.cpp
    FastClone/protocol_codec.cpp
    FastClone/win_socket.cpp
)
target_include_directories(FastCloneRouteProbe PRIVATE FastClone)
target_link_libraries(FastCloneRouteProbe PRIVATE Threads::Threads)

if(WIN32)
    target_compile_definitions(FastCloneRouteProbe PRIVATE
        _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(FastCloneRouteProbe PRIVATE ws2_32 iphlpapi bcrypt)
endif()
