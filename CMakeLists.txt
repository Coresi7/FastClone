cmake_minimum_required(VERSION 3.16)
project(FastClone LANGUAGES CXX)

include(GNUInstallDirs)

# Version is authored in FastClone/version.h (single source of truth, consumed by both
# the hand-maintained VS solution and this CMake build). Parse it here for metadata so
# the zip name, the binary's --version, and the release tag all stay in lock-step.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/FastClone/version.h" _fc_version_line
     REGEX "kFastCloneVersion[ \t]*=[ \t]*\"[^\"]+\"")
if(_fc_version_line)
    string(REGEX MATCH "\"([^\"]+)\"" _fc_version_match "${_fc_version_line}")
    set(FASTCLONE_VERSION "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "Cannot parse FastClone/version.h: kFastCloneVersion not found.")
endif()
message(STATUS "FastClone version: ${FASTCLONE_VERSION}")

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
    FastClone/compare_phase.cpp
    FastClone/compare_pipeline.cpp
    FastClone/delta.cpp
    FastClone/file_index.cpp
    FastClone/hash_memcache.cpp
    FastClone/main.cpp
    FastClone/net_topology.cpp
    FastClone/parallel_dir_walk.cpp
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
    FastClone/compare_phase.cpp
    FastClone/compare_pipeline.cpp
    FastClone/delta.cpp
    FastClone/file_index.cpp
    FastClone/hash_memcache.cpp
    FastClone/net_topology.cpp
    FastClone/parallel_dir_walk.cpp
    FastClone/protocol.cpp
    FastClone/protocol_codec.cpp
    FastClone/sync_util.cpp
    FastClone/transfer_tuning.cpp
    FastClone/win_socket.cpp
    ${FASTCLONE_DISK_IO_SOURCES}
    tests/test_main.cpp
    tests/test_cli.cpp
    tests/test_compare_phase.cpp
    tests/test_compare_pipeline.cpp
    tests/test_protocol_codec.cpp
    tests/test_delta.cpp
    tests/test_streaming_buildplan.cpp
    tests/test_disk_io_align.cpp
    tests/test_disk_io_driver.cpp
    tests/test_read_gate.cpp
    tests/test_file_index.cpp
    tests/test_manifest_dirent.cpp
    tests/test_sync_util.cpp
    tests/test_hash_memcache.cpp
    tests/test_reconnect.cpp
    tests/test_route_selection.cpp
    tests/test_link_scheduler.cpp
    tests/test_wan_tuning.cpp
    tests/test_write_path_accounting.cpp
)
target_include_directories(FastCloneTests PRIVATE FastClone)
target_link_libraries(FastCloneTests PRIVATE Threads::Threads)
# CMake build includes test_cli.cpp (RunCliTests is defined); enable its call in test_main.cpp.
# The VS FastCloneTests.vcxproj excludes test_cli.cpp and leaves this macro undefined.
target_compile_definitions(FastCloneTests PRIVATE FASTCLONE_ENABLE_CLI_TESTS)
# S-04 (AC-27/B13): inject the cli.cpp source path so the static include-scan test can assert the
# translation unit carries no unused #include "sync_util.h".
target_compile_definitions(FastCloneTests PRIVATE
    FASTCLONE_CLI_SRC=${CMAKE_CURRENT_SOURCE_DIR}/FastClone/cli.cpp)

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

# Data-integrity integration test (Windows only): end-to-end SHA256 byte-for-byte
# verification of server-transferred files. Covers the C2 "传输字节逐字节不变" guarantee
# that unit tests cannot reach (sync_engine_server.cpp is not in the FastCloneTests target).
# Runs via ctest; requires FastClone.exe to be built first.
if(WIN32)
    add_test(NAME data_integrity_integration
        COMMAND powershell -ExecutionPolicy Bypass -File
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/data_integrity_integration.ps1
                -ExePath $<TARGET_FILE:FastClone>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
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

# --- FastCheck: 只读目录比对独立工具（fastcheck；additive，不触碰 FastClone/FastCloneTests） ---
# 依赖闭包：check 源 + 共享 compare_phase + file_index(ComputeFileHash/ComputeFileHashViaDriver,+xxhash) +
# client_handshake/protocol/protocol_codec/win_socket。明确不链 sync_engine_client/server/shared、
# delta、main、transfer_tuning、hash_memcache、net_topology、cli（FR-33/AC-42）。
# FastCheck 复用 DiskIoDriver + SequentialReader 执行并行本地哈希（ComputeFileHashViaDriver），
# 因此链接统一 disk_io 五件套（${FASTCLONE_DISK_IO_SOURCES}）；file_index.cpp 依赖 disk_io，凡链它的
# target 必须链 disk_io（fastcheck-parallel-hash M4/FR-03/D-07）。
set(FASTCHECK_SHARED_SOURCES
    FastCheck/check_cli.cpp
    FastCheck/check_engine.cpp
    FastCheck/check_report.cpp
    FastClone/compare_phase.cpp
    FastClone/compare_pipeline.cpp
    FastClone/file_index.cpp
    FastClone/client_handshake.cpp
    FastClone/parallel_dir_walk.cpp
    FastClone/protocol.cpp
    FastClone/protocol_codec.cpp
    FastClone/sync_util.cpp
    FastClone/win_socket.cpp
    ${FASTCLONE_DISK_IO_SOURCES}
)

# xxhash / Win 链接与 FastClone 复用同一解析结果，封装成函数避免在两个 target 上重复。
function(fastcheck_configure_target tgt)
    target_include_directories(${tgt} PRIVATE FastClone FastCheck)
    target_link_libraries(${tgt} PRIVATE Threads::Threads)
    if(XXHASH_FOUND)
        target_include_directories(${tgt} PRIVATE ${XXHASH_INCLUDE_DIRS})
        target_link_directories(${tgt} PRIVATE ${XXHASH_LIBRARY_DIRS})
        target_link_libraries(${tgt} PRIVATE ${XXHASH_LIBRARIES})
        target_compile_options(${tgt} PRIVATE ${XXHASH_CFLAGS_OTHER})
    else()
        target_include_directories(${tgt} PRIVATE ${XXHASH_INCLUDE_DIR})
        target_link_libraries(${tgt} PRIVATE ${XXHASH_LIBRARY})
    endif()
    if(WIN32)
        target_compile_definitions(${tgt} PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
        # win_socket 需 ws2_32；无 net_topology 故不需 iphlpapi/bcrypt。
        target_link_libraries(${tgt} PRIVATE ws2_32)
    endif()
endfunction()

add_executable(FastCheck
    FastCheck/check_main.cpp
    ${FASTCHECK_SHARED_SOURCES}
)
fastcheck_configure_target(FastCheck)
install(TARGETS FastCheck RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# FastCheckTests：链接同 FastCheck（去 check_main.cpp，加测试入口），兼作 FastCheck link 闭包冒烟。
add_executable(FastCheckTests
    ${FASTCHECK_SHARED_SOURCES}
    FastCheck/tests/test_main.cpp
    FastCheck/tests/test_check_engine.cpp
    FastCheck/tests/test_check_report.cpp
    FastCheck/tests/test_check_cli.cpp
)
fastcheck_configure_target(FastCheckTests)
add_test(NAME FastCheckTests COMMAND FastCheckTests)

# Optional io_uring (Linux only). When liburing is present the io_uring backend compiles and links
# against it; when absent, disk_io_backend_uring.cpp's __has_include guard leaves only the bounded
# pread/pwrite pool fallback (design §7.3). macOS/Windows never look for liburing. Placed after all
# targets are defined so FastCheck/FastCheckTests (which now link the disk_io five-piece set) exist
# when target_link_libraries runs (fastcheck-parallel-hash D-07).
if(UNIX AND NOT APPLE)
    find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)
    find_library(LIBURING_LIBRARY NAMES uring)
    if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
        foreach(tgt FastClone FastCloneTests FastCheck FastCheckTests)
            target_include_directories(${tgt} PRIVATE ${LIBURING_INCLUDE_DIR})
            target_link_libraries(${tgt} PRIVATE ${LIBURING_LIBRARY})
        endforeach()
        message(STATUS "FastClone: io_uring backend enabled (liburing found)")
    else()
        message(STATUS "FastClone: liburing not found; Linux disk IO uses the pread/pwrite pool")
    endif()
endif()
