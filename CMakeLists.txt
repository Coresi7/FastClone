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

if(MSVC)
    # /utf-8: source files are UTF-8 encoded. Placed here (rather than in
    # -DCMAKE_CXX_FLAGS) to preserve MSVC's default /EHsc. Overriding
    # CMAKE_CXX_FLAGS on the command line would drop /EHsc, breaking exception
    # unwinding (C4530): RAII locks/thread destructors would not run and test
    # processes could deadlock. /EHsc is given explicitly so standard exception
    # unwinding is always enabled.
    add_compile_options(/utf-8 /EHsc)
endif()

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
    FastClone/extra_scan.cpp
    FastClone/file_index.cpp
    FastClone/hash_memcache.cpp
    FastClone/local_probe.cpp
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
    FastClone/extra_scan.cpp
    FastClone/file_index.cpp
    FastClone/hash_memcache.cpp
    FastClone/local_probe.cpp
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
    tests/test_shared_probe.cpp
    tests/test_shared_extra_scan.cpp
    tests/test_shared_source_gate.cpp
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
# unify-probe-extra-shared (AC-05, form A): let the source-gate test locate the repo root
# so it can scan sync_engine_client.cpp / check_engine.cpp for residual probe primitives.
target_compile_definitions(FastCloneTests PRIVATE
    FASTCLONE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

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
# verification of server-transferred files. Covers the C2 "transferred bytes are
# byte-for-byte unchanged" guarantee
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

# --- FastCheck: standalone read-only directory comparison tool (additive; does not
#     touch FastClone/FastCloneTests) ---
# Dependency closure: check sources + shared compare_phase + file_index
# (ComputeFileHash/ComputeFileHashViaDriver, +xxhash) + client_handshake/protocol/
# protocol_codec/win_socket. Explicitly does NOT link sync_engine_client/server/
# shared, delta, main, transfer_tuning, hash_memcache, net_topology, cli
# (FR-33/AC-42).
# FastCheck reuses DiskIoDriver + SequentialReader for parallel local hashing
# (ComputeFileHashViaDriver), so it links the unified disk_io set
# (${FASTCLONE_DISK_IO_SOURCES}); file_index.cpp depends on disk_io, so any target
# linking it must also link disk_io (fastcheck-parallel-hash M4/FR-03/D-07).
set(FASTCHECK_SHARED_SOURCES
    FastCheck/check_cli.cpp
    FastCheck/check_engine.cpp
    FastCheck/check_report.cpp
    FastClone/compare_phase.cpp
    FastClone/compare_pipeline.cpp
    FastClone/extra_scan.cpp
    FastClone/file_index.cpp
    FastClone/client_handshake.cpp
    FastClone/local_probe.cpp
    FastClone/parallel_dir_walk.cpp
    FastClone/protocol.cpp
    FastClone/protocol_codec.cpp
    FastClone/sync_util.cpp
    FastClone/win_socket.cpp
    ${FASTCLONE_DISK_IO_SOURCES}
)

# xxhash / Win linking reuses the same resolution as FastClone; wrapped in a
# function to avoid duplication across the two targets.
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
        # win_socket needs ws2_32; no net_topology, so iphlpapi/bcrypt are not needed.
        target_link_libraries(${tgt} PRIVATE ws2_32)
    endif()
endfunction()

add_executable(FastCheck
    FastCheck/check_main.cpp
    ${FASTCHECK_SHARED_SOURCES}
)
fastcheck_configure_target(FastCheck)
install(TARGETS FastCheck RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# FastCheckTests: links like FastCheck (minus check_main.cpp, plus the test entry
# point); also serves as a FastCheck link-closure smoke test.
add_executable(FastCheckTests
    ${FASTCHECK_SHARED_SOURCES}
    FastCheck/tests/test_main.cpp
    FastCheck/tests/test_check_engine.cpp
    FastCheck/tests/test_check_report.cpp
    FastCheck/tests/test_check_cli.cpp
    FastCheck/tests/test_shared_probe.cpp
    FastCheck/tests/test_shared_extra_scan.cpp
)
fastcheck_configure_target(FastCheckTests)
# unify-probe-extra-shared-converge: the shared test fixture header (test_fixture_support.h)
# lives in tests/. Added to FastCheckTests ONLY (after the shared configure call) so the
# FastCheck production target's include path carries no test directories.
target_include_directories(FastCheckTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME FastCheckTests COMMAND FastCheckTests)

# unify-probe-extra-shared (FR-13): process-level end-to-end consistency invariant
# "FastClone sync exit 0 => FastCheck (fast AND strict) diff=0 missing=0 extra_local=0".
# Windows carrier is registered here; the POSIX carrier (tests/clone_check_consistency.sh)
# is CI/manual-invocation only, same as tests/data_integrity_integration.sh (design §7.2).
if(WIN32)
    add_test(NAME clone_check_consistency
        COMMAND powershell -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/clone_check_consistency.ps1
                -ExePath $<TARGET_FILE:FastClone>
                -CheckExePath $<TARGET_FILE:FastCheck>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(clone_check_consistency PROPERTIES
        DEPENDS data_integrity_integration
        TIMEOUT 900)
endif()

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
