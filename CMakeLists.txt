cmake_minimum_required(VERSION 3.16)
project(FastClone LANGUAGES CXX)

include(GNUInstallDirs)

# --- Auto-incrementing version (cmake-auto-version, design §3.1/§3.3 · TL option A revised) ---
# FastClone/version.h is the runtime single source of truth for the version string (consumed by
# cli.cpp / check_cli.cpp for --version, and validated by the release workflow drift guard).
#
# IMPORTANT (option A revised): version.h is an OUTPUT in EVERY scenario. It is NEVER read as a
# base input. The local development base comes from the persistent file `.fcversion`, which has
# two lines:
#     base=1.0.0
#     counter=5
# `.fcversion` lives at the repo root, is gitignored, and is a local-only state file.
#
# configure-time logic:
#   if EXISTS .fcversion:                  # local development
#       read base, counter
#       counter = counter + 1
#       version = <base.major>.<base.minor>.<base.patch + counter>
#       write .fcversion (base unchanged, only counter updated)
#   else:                                  # CI / clean checkout
#       base = parsed committed version.h (1.0.0 on a clean checkout)
#       c    = git rev-list --count HEAD   (a relative offset from that base)
#       version = <base.major>.<base.minor>.<base.patch + c>
#       DO NOT write .fcversion            (keeps clean checkouts reproducible, nothing staged)
#
# In both cases version.h is only rewritten as output. Only the string value of
# kFastCloneVersion is changed; the header's structure (namespace fc { constexpr const char*
# kFastCloneVersion = "..."; }) is preserved, so all consumers and the regex parse below keep
# working unchanged. Known side effect: version.h is rewritten every configure, so the working
# tree shows dirty continuously; the user decides when to commit (this build never commits).
set(FC_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/FastClone/version.h")
set(FC_VERSION_COUNTER_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.fcversion")

# Helper: split a MAJOR.MINOR.PATCH string into three vars; FATAL on bad shape.
function(fc_split_version _in _maj _min _pat)
    string(REPLACE "." ";" _parts "${_in}")
    list(LENGTH _parts _n)
    if(_n LESS 3)
        message(FATAL_ERROR "FastClone version '${_in}' must be in MAJOR.MINOR.PATCH form.")
    endif()
    list(GET _parts 0 _m)
    list(GET _parts 1 _n2)
    list(GET _parts 2 _p)
    set(${_maj} "${_m}" PARENT_SCOPE)
    set(${_min} "${_n2}" PARENT_SCOPE)
    set(${_pat} "${_p}" PARENT_SCOPE)
endfunction()

# 1) Determine the base version and the counter, per scenario.
if(EXISTS "${FC_VERSION_COUNTER_FILE}")
    # ---- Local development: base + counter come from .fcversion; version.h is output only. ----
    # Read the whole file and extract base=/counter= with two regexes (no foreach: foreach body
    # runs in a child scope, which made the parsed values disappear). Normalize CRLF first so a
    # Windows-written .fcversion parses identically to an LF one.
    file(READ "${FC_VERSION_COUNTER_FILE}" _fc_fccontent)
    string(REPLACE "\r\n" "\n" _fc_fccontent "${_fc_fccontent}")
    string(REPLACE "\r" "\n" _fc_fccontent "${_fc_fccontent}")
    set(_fc_base "")
    set(_fc_counter_raw "")
    if(_fc_fccontent MATCHES "(^|\n)[ \t]*base[ \t]*=[ \t]*([0-9]+\.[0-9]+\.[0-9]+)[ \t]*(\n|$)")
        set(_fc_base "${CMAKE_MATCH_2}")
    endif()
    if(_fc_fccontent MATCHES "(^|\n)[ \t]*counter[ \t]*=[ \t]*([0-9]+)[ \t]*(\n|$)")
        set(_fc_counter_raw "${CMAKE_MATCH_2}")
    endif()
    if(NOT _fc_base MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Auto-version: .fcversion has invalid/missing 'base=' (need MAJOR.MINOR.PATCH).")
    endif()
    if(NOT _fc_counter_raw MATCHES "^[0-9]+$")
        message(FATAL_ERROR "Auto-version: .fcversion has invalid/missing 'counter=' (need a non-negative integer).")
    endif()
    fc_split_version("${_fc_base}" FC_VERSION_MAJOR FC_VERSION_MINOR FC_BASE_PATCH)
    math(EXPR FC_COUNTER "${_fc_counter_raw} + 1")
    set(FC_COUNTER_SOURCE ".fcversion")
    # Persist the incremented counter; base stays unchanged.
    file(WRITE "${FC_VERSION_COUNTER_FILE}" "base=${_fc_base}\ncounter=${FC_COUNTER}\n")
else()
    # ---- No .fcversion (CI / clean checkout): base MUST come from the COMMITTED version.h,
    #      never from the working-tree header (which a previous configure may have rewritten).
    #      This is the P0-2 fix: reading the possibly-rewritten working-tree version.h as base
    #      made two clean configures double-accumulate (1.0.0 -> 1.0.180 -> 1.0.360). Reading
    #      `git show HEAD:...version.h` is immune to working-tree rewrites (design §2 S1). ----
    find_program(FC_GIT_EXE NAMES git)
    set(_fc_base "")
    if(FC_GIT_EXE)
        execute_process(
            COMMAND ${FC_GIT_EXE} show HEAD:FastClone/version.h
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _fc_head_content
            ERROR_QUIET
            RESULT_VARIABLE _fc_git_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_fc_git_rc EQUAL 0 AND _fc_head_content MATCHES "kFastCloneVersion[ \t]*=[ \t]*\"([^\"]+)\"")
            set(_fc_base "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(NOT _fc_base MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        # Fallback: git unavailable / not a git checkout / parse failed. Degrade to 1.0.0 base
        # (do NOT FATAL) so a non-git tarball still configures, albeit without auto-increment
        # fidelity. counter stays 0 for this one-shot path.
        set(_fc_base "1.0.0")
        message(WARNING "Auto-version: cannot read committed version.h from git; falling back to base=1.0.0.")
    endif()
    fc_split_version("${_fc_base}" FC_VERSION_MAJOR FC_VERSION_MINOR FC_BASE_PATCH)
    # First clean configure: start the local counter at 1 and persist .fcversion so every
    # subsequent configure takes the monotonic local branch (counter+1) -> 1.0.1, 1.0.2, ...
    # Without persisting, each clean configure would repeat the same git-derived value and could
    # not satisfy AC-01's per-configure +1 cadence (design §2 S1 step 2).
    set(FC_COUNTER 1)
    set(FC_COUNTER_SOURCE "git-base-init")
    file(WRITE "${FC_VERSION_COUNTER_FILE}" "base=${_fc_base}\ncounter=${FC_COUNTER}\n")
endif()

# 2) Compute the new patch = base_patch + counter (monotonic, never resets).
math(EXPR FC_NEW_PATCH "${FC_BASE_PATCH} + ${FC_COUNTER}")
set(FC_NEW_VERSION "${FC_VERSION_MAJOR}.${FC_VERSION_MINOR}.${FC_NEW_PATCH}")

# 3) Write the new version back into version.h (OUTPUT only; low-noise: only when changed).
#    IMPORTANT: the string currently sitting in version.h is NOT necessarily _fc_base — in the
#    local branch version.h was already rewritten to the previous build's result (e.g. "1.0.1")
#    while .fcversion.base stayed at "1.0.0". So we always re-parse the CURRENT value out of
#    version.h and replace exactly that, never _fc_base.
file(READ "${FC_VERSION_HEADER}" _fc_header_content)
file(STRINGS "${FC_VERSION_HEADER}" _fc_cur_line
     REGEX "kFastCloneVersion[ \t]*=[ \t]*\"[^\"]+\"")
string(REGEX MATCH "\"([^\"]+)\"" _fc_cur_match "${_fc_cur_line}")
set(_fc_cur_in_header "${CMAKE_MATCH_1}")
string(FIND "${_fc_header_content}" "${_fc_cur_in_header}" _fc_have_old)
if(_fc_have_old GREATER_EQUAL 0)
    string(REPLACE "${_fc_cur_in_header}" "${FC_NEW_VERSION}" _fc_new_content "${_fc_header_content}")
    if(NOT _fc_new_content STREQUAL "${_fc_header_content}")
        file(WRITE "${FC_VERSION_HEADER}" "${_fc_new_content}")
        message(STATUS "Auto-version: rewrote version.h ${_fc_cur_in_header} -> ${FC_NEW_VERSION} (counter=${FC_COUNTER} from ${FC_COUNTER_SOURCE})")
    else()
        message(STATUS "Auto-version: version.h already at ${FC_NEW_VERSION}; no change")
    endif()
endif()

set(FASTCLONE_VERSION "${FC_NEW_VERSION}")
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
    # unwinding is always enabled. Applies to both cl.exe and clang-cl.
    add_compile_options(/utf-8 /EHsc)
    # Whole Program Optimization for genuine MSVC (cl.exe) non-Debug configs.
    # Release builds get /GL compile + /LTCG link for cross-TU optimization.
    # clang-cl is excluded here: it already gets cross-TU optimization via ThinLTO in the
    # Clang branch below, and /GL must not be mixed with -flto=thin.
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options($<$<NOT:$<CONFIG:Debug>>:/GL>)
        add_link_options($<$<NOT:$<CONFIG:Debug>>:/LTCG>)
    endif()
endif()

# --- Compiler performance tuning for clang / clang-cl ---
# clang-cl reports CMAKE_CXX_COMPILER_ID "Clang" while defining MSVC=ON.
# The VS generator is multi-config (CMAKE_BUILD_TYPE empty), so we scope
# optimization flags to non-Debug configs via generator expressions.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(MSVC)
        # clang-cl driver: MSVC-style flags + /clang: passthrough for LLVM opts.
        # /O2 speed + /Ob2 aggressive inline; -march/-mtune=native enables host
        # SSE4.2/AVX2 (helps xxhash & byte comparisons); ThinLTO via lld-link.
        # Applied for non-Debug single-config builds (e.g. -DCMAKE_BUILD_TYPE=Release).
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_options(/O2 /Ob2 /clang:-march=native /clang:-mtune=native /clang:-flto=thin)
            # NOTE: CMake links via lld-link.exe directly (not the clang-cl driver),
            # so linker opts must be plain lld-link flags, not /clang: passthrough.
            # -flto=thin matches the compile-time bitcode; linker is already lld-link.
            add_link_options(-flto=thin)
        endif()
    else()
        # Plain clang (Linux/macOS): -O3 + ThinLTO.
        #
        # Scoped with a plain if(), NOT with $<NOT:$<CONFIG:Debug>>: this branch
        # only runs under single-config generators (Unix Makefiles / Ninja), where
        # CMAKE_BUILD_TYPE is authoritative — the same form the clang-cl branch
        # above already uses. Pipeline #46 proved the generator-expression form is
        # unsafe here: with CMAKE_INTERPROCEDURAL_OPTIMIZATION ON, the Unix
        # Makefiles generator emitted the half-evaluated expression into flags.make
        # and clang++ received the literal tokens '$<1:-O3' and '-flto=thin>'.
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            # ThinLTO is supplied by IPO below; passing -flto=thin by hand
            # duplicated it and was the token that carried the leaked '>'.
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
            # No -march/-mtune=native: it bakes the BUILD machine's CPU into the
            # artifacts we ship, and on the darwin runner (amd64 under Rosetta on
            # Apple Silicon) `native` resolves to 'apple-m3', which this
            # x86-targeting clang rejects: error: unknown target CPU 'apple-m3'.
            add_compile_options(-O3)
        endif()
    endif()
    # PGO (two-phase) — enable after collecting a profdata from a real run:
    #   configure -DFASTCLONE_PGO=gen, run a representative workload, then
    #   -DFASTCLONE_PGO=use -DFASTCLONE_PROFDATA=<path>/fastclone.profdata
    # if(FASTCLONE_PGO STREQUAL "gen")
    #     add_compile_options($<$<NOT:$<CONFIG:Debug>>:/clang:-fprofile-generate>)
    # elseif(FASTCLONE_PGO STREQUAL "use" AND FASTCLONE_PROFDATA)
    #     add_compile_options($<$<NOT:$<CONFIG:Debug>>:/clang:-fprofile-use=${FASTCLONE_PROFDATA}>)
    # endif()
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

# --- Static library layering (cmake-only-migration, design §4) ---
# Each .cpp compiles exactly once: shared impls in fc_core, FastClone-only transfer
# engine in fc_sync, FastCheck-only check engine in fc_check. Executables link the
# libraries they need instead of re-compiling shared sources.
find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(XXHASH QUIET libxxhash xxhash)
endif()

if(XXHASH_FOUND)
    set(FC_XXHASH_INCLUDE_DIRS ${XXHASH_INCLUDE_DIRS})
    set(FC_XXHASH_LIBRARY_DIRS ${XXHASH_LIBRARY_DIRS})
    set(FC_XXHASH_LIBRARIES ${XXHASH_LIBRARIES})
    set(FC_XXHASH_CFLAGS ${XXHASH_CFLAGS_OTHER})
else()
    find_library(XXHASH_LIBRARY NAMES xxhash libxxhash)
    if(NOT XXHASH_LIBRARY)
        message(FATAL_ERROR "xxhash library not found. Install libxxhash development package.")
    endif()
    find_path(XXHASH_INCLUDE_DIR NAMES xxhash.h)
    if(NOT XXHASH_INCLUDE_DIR)
        message(FATAL_ERROR "xxhash headers not found. Install libxxhash development package.")
    endif()
    set(FC_XXHASH_INCLUDE_DIRS ${XXHASH_INCLUDE_DIR})
    set(FC_XXHASH_LIBRARIES ${XXHASH_LIBRARY})
endif()
message(STATUS "FastClone: xxhash include=${FC_XXHASH_INCLUDE_DIRS} lib=${FC_XXHASH_LIBRARIES}")

# P0-1 fix helper: apply the resolved xxhash header path to library targets. Library
# sources (fc_core/delta.cpp, fc_core/file_index.cpp) #include "xxhash.h", but previously
# only executable targets received the include directory. On a clean environment (vcvars
# reset INCLUDE, no cl.exe fallback) that left library TUs with a fatal C1083; the old
# builds only passed because cl.exe fell back to the INCLUDE env var. Libraries need the
# header path only — they do not link xxhash (the symbol is pulled in by the exe).
# Defined HERE, before the library targets below, because CMake requires a function to be
# defined before it is called.
function(fc_apply_xxhash_includes tgt)
    target_include_directories(${tgt} PRIVATE ${FC_XXHASH_INCLUDE_DIRS})
endfunction()

# fc_core: all shared implementations (no main()). Every consumer links this.
add_library(fc_core STATIC
    FastClone/file_index.cpp
    FastClone/compare_phase.cpp
    FastClone/compare_pipeline.cpp
    FastClone/delta.cpp
    FastClone/extra_scan.cpp
    FastClone/local_probe.cpp
    FastClone/parallel_dir_walk.cpp
    FastClone/hash_memcache.cpp
    FastClone/sync_util.cpp
    FastClone/transfer_tuning.cpp
    FastClone/protocol.cpp
    FastClone/protocol_codec.cpp
    FastClone/client_handshake.cpp
    FastClone/net_topology.cpp
    FastClone/win_socket.cpp
    FastClone/cli.cpp
    ${FASTCLONE_DISK_IO_SOURCES}
)
# fc_core sources live under FastClone/ and reference each other's headers by bare
# name (e.g. file_index.h, version.h); the library target must carry the include path.
target_include_directories(fc_core PRIVATE FastClone)
# P0-1: fc_core sources (delta.cpp, file_index.cpp) #include "xxhash.h"; give the library
# target the xxhash header path so a clean environment (no INCLUDE fallback) compiles it.
fc_apply_xxhash_includes(fc_core)

# fc_sync: FastClone transfer engine only.
add_library(fc_sync STATIC
    FastClone/sync_engine_shared.cpp
    FastClone/sync_engine_server.cpp
    FastClone/sync_engine_client.cpp
)
target_include_directories(fc_sync PRIVATE FastClone)
# P0-1: keep the xxhash header path consistent across library targets (defensive; fc_sync
# sources may transitively include headers that reference xxhash).
fc_apply_xxhash_includes(fc_sync)

# fc_check: FastCheck check engine only. Its sources reference FastClone/ headers
# (version.h, compare_phase.h, ...), so it needs both FastClone and FastCheck paths.
add_library(fc_check STATIC
    FastCheck/check_cli.cpp
    FastCheck/check_engine.cpp
    FastCheck/check_report.cpp
)
target_include_directories(fc_check PRIVATE FastClone FastCheck)
# P0-1: fc_check sources reference FastClone/ headers that pull in xxhash.h; give the library
# target the xxhash header path so a clean environment compiles it.
fc_apply_xxhash_includes(fc_check)

# Single configure entry point for every final target (design §4.3). Consumes the
# already-resolved xxhash result; applies include dirs, Threads, platform macros and
# system libraries uniformly so the four duplicated target_* blocks collapse to one.
function(fc_configure_target tgt)
    target_include_directories(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/FastClone ${CMAKE_SOURCE_DIR}/FastCheck)
    target_link_libraries(${tgt} PRIVATE Threads::Threads)
    if(XXHASH_FOUND)
        target_include_directories(${tgt} PRIVATE ${FC_XXHASH_INCLUDE_DIRS})
        target_link_directories(${tgt} PRIVATE ${FC_XXHASH_LIBRARY_DIRS})
        target_link_libraries(${tgt} PRIVATE ${FC_XXHASH_LIBRARIES})
        target_compile_options(${tgt} PRIVATE ${FC_XXHASH_CFLAGS})
    else()
        target_include_directories(${tgt} PRIVATE ${FC_XXHASH_INCLUDE_DIRS})
        target_link_libraries(${tgt} PRIVATE ${FC_XXHASH_LIBRARIES})
    endif()
    if(WIN32)
        target_compile_definitions(${tgt} PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endfunction()

add_executable(FastClone
    FastClone/main.cpp
)
fc_configure_target(FastClone)
target_link_libraries(FastClone PRIVATE fc_sync fc_core)
if(WIN32)
    target_link_libraries(FastClone PRIVATE ws2_32 iphlpapi bcrypt)
endif()

enable_testing()

# FastCloneTests: only the test entry + case sources; shared impls come from fc_core.
# cli.cpp lives in fc_core (design §4.2); the CLI test case is enabled via the macro
# below and linked from the library (static archive exports RunCliTests symbol).
add_executable(FastCloneTests
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
# CMake build includes test_cli.cpp (RunCliTests is defined); enable its call in test_main.cpp.
target_compile_definitions(FastCloneTests PRIVATE FASTCLONE_ENABLE_CLI_TESTS)
# S-04 (AC-27/B13): inject the cli.cpp source path so the static include-scan test can assert the
# translation unit carries no unused #include "sync_util.h".
target_compile_definitions(FastCloneTests PRIVATE
    FASTCLONE_CLI_SRC=${CMAKE_CURRENT_SOURCE_DIR}/FastClone/cli.cpp)
# unify-probe-extra-shared (AC-05, form A): let the source-gate test locate the repo root
# so it can scan sync_engine_client.cpp / check_engine.cpp for residual probe primitives.
target_compile_definitions(FastCloneTests PRIVATE
    FASTCLONE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
fc_configure_target(FastCloneTests)
# net_topology.cpp / win_socket.cpp (in fc_core) pull in Winsock, IP Helper and CNG.
if(WIN32)
    target_link_libraries(FastCloneTests PRIVATE ws2_32 iphlpapi bcrypt)
endif()
target_link_libraries(FastCloneTests PRIVATE fc_core)
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
                -Port 27894
                -BindLoopback
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

install(TARGETS FastClone
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# --- Diagnostics-only multipath route probe (additive; does not touch FastClone/Tests) ---
# Design §4.1/§4.2: links fc_core (shared prims incl. client_handshake/net_topology/
# protocol/win_socket). The extra .obj pulled from fc_core (e.g. file_index, cli) are
# harmless for this diagnostics tool; static archive按需抽取 keeps unused symbols out.
add_executable(FastCloneRouteProbe
    FastClone/route_probe_main.cpp
)
fc_configure_target(FastCloneRouteProbe)
if(WIN32)
    target_link_libraries(FastCloneRouteProbe PRIVATE ws2_32 iphlpapi bcrypt)
endif()
target_link_libraries(FastCloneRouteProbe PRIVATE fc_core)

# --- FastCheck: standalone read-only directory comparison tool (additive; does not
#     touch FastClone/FastCloneTests) ---
# Design §4.1: FastCheck = check_main.cpp + fc_check + fc_core. fc_core provides the
# shared compare/file_index/handshake/disk_io prims; fc_check provides the check engine.
# cli.cpp is in fc_core but FastCheck never references fc::ParseCli, so the static
# archive does not pull it in (design §4.2): FR-33/AC-42 link-closure is preserved.
add_executable(FastCheck
    FastCheck/check_main.cpp
)
fc_configure_target(FastCheck)
if(WIN32)
    # win_socket (in fc_core) needs ws2_32; FastCheck has no net_topology, so no iphlpapi/bcrypt.
    target_link_libraries(FastCheck PRIVATE ws2_32)
endif()
target_link_libraries(FastCheck PRIVATE fc_check fc_core)
install(TARGETS FastCheck RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# FastCheckTests: links like FastCheck (minus check_main.cpp, plus the test entry
# point); also serves as a FastCheck link-closure smoke test.
add_executable(FastCheckTests
    FastCheck/tests/test_main.cpp
    FastCheck/tests/test_check_engine.cpp
    FastCheck/tests/test_check_report.cpp
    FastCheck/tests/test_check_cli.cpp
    FastCheck/tests/test_shared_probe.cpp
    FastCheck/tests/test_shared_extra_scan.cpp
)
fc_configure_target(FastCheckTests)
if(WIN32)
    target_link_libraries(FastCheckTests PRIVATE ws2_32)
endif()
target_link_libraries(FastCheckTests PRIVATE fc_check fc_core)
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
                -Port 27941
                -BindLoopback
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(clone_check_consistency PROPERTIES
        DEPENDS data_integrity_integration
        TIMEOUT 900)

    # cmake-only-migration (design §7): register the previously-orphan integration
    # scripts as ctest cases. Each takes -ExePath pointing at the freshly built
    # FastClone.exe. file_range_tcp_proxy.ps1 is a helper spawned by
    # file_range_integration.ps1 (not a standalone test: it has no assertions and
    # requires -ListenPort/-TargetPort), so it is deliberately NOT registered here.
    add_test(NAME file_range_integration
        COMMAND powershell -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_range_integration.ps1
                -ExePath $<TARGET_FILE:FastClone>
                -Port 27910
                -BindLoopback
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(file_range_integration PROPERTIES TIMEOUT 600)

    add_test(NAME once_multi_integration
        COMMAND powershell -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/once_multi_integration.ps1
                -ExePath $<TARGET_FILE:FastClone>
                -Port 27896
                -BindLoopback
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(once_multi_integration PROPERTIES TIMEOUT 600)

    add_test(NAME once_server_integration
        COMMAND powershell -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/once_server_integration.ps1
                -ExePath $<TARGET_FILE:FastClone>
                -Port 27893
                -BindLoopback
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(once_server_integration PROPERTIES TIMEOUT 600)

    add_test(NAME reconnect_integration
        COMMAND powershell -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/reconnect_integration.ps1
                -ExePath $<TARGET_FILE:FastClone>
                -Port 27892
                -BindLoopback
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(reconnect_integration PROPERTIES TIMEOUT 600)

    add_test(NAME wait_connect_timeout_integration
        COMMAND powershell -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/wait_connect_timeout_integration.ps1
                -ExePath $<TARGET_FILE:FastClone>
                -Port 27895
                -BindLoopback
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(wait_connect_timeout_integration PROPERTIES TIMEOUT 600)

    add_test(NAME server_memory_retention_probe
        COMMAND powershell -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_memory_retention_probe.ps1
                -ExePath $<TARGET_FILE:FastClone>
                -Port 27931
                -BindLoopback
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(server_memory_retention_probe PROPERTIES TIMEOUT 600)
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
