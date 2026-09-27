cmake_minimum_required(VERSION 3.20)
project(psvr2_krw_c VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(PSVR2_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(PSVR2_BUILD_TOOLS "Build the standalone native helper tools" ON)
option(PSVR2_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(PSVR2_FIRMWARE_FIXTURES_DIR "" CACHE PATH
    "External private firmware fixtures for optional exact-image tests")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

function(psvr2_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
        $<$<AND:$<BOOL:${PSVR2_WARNINGS_AS_ERRORS}>,$<C_COMPILER_ID:AppleClang,Clang,GNU>>:-Werror>
    )
endfunction()

add_library(psvr2krw
    src/arm64.c
    src/auth.c
    src/audit.c
    src/constants.c
    src/device.c
    src/exploit.c
    src/gaze.c
    src/kernel_bootstrap.c
    src/kernel_bootstrap_payload.c
    src/kernel_exec.c
    src/kernel_images.c
    src/kernel_list.c
    src/payload_bootstrap.c
    src/payload_runtime.c
    src/kernel_runtime.c
    src/rw.c
    src/tasks.c
    src/umh.c
    src/upload.c
    src/vfs.c
    src/memory.c
    src/module_helper.c
    src/stage1.c
    src/stage1_discovery.c
    src/util.c
)
target_include_directories(psvr2krw PUBLIC include)
target_link_libraries(psvr2krw PUBLIC PkgConfig::LIBUSB Threads::Threads)
target_compile_definitions(psvr2krw PRIVATE _FILE_OFFSET_BITS=64)
psvr2_enable_warnings(psvr2krw)

add_library(psvr2krw_cli
    src/line_editor.c
    src/shell.c
    src/shell_busybox.c
    src/shell_completion.c
    src/shell_jobs.c
    src/shell_memory.c
    src/shell_persistence.c
    src/shell_stage1.c
    src/shell_bulk.c
    src/shell_emmc.c
    src/shell_vfs.c
)
target_include_directories(psvr2krw_cli PUBLIC include)
target_include_directories(psvr2krw_cli PRIVATE src)
target_link_libraries(psvr2krw_cli PUBLIC psvr2krw)
psvr2_enable_warnings(psvr2krw_cli)

if(PSVR2_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    target_compile_options(psvr2krw PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(psvr2krw PUBLIC -fsanitize=address,undefined)
endif()

add_executable(psvr2_krw_c src/main.c)
target_sources(psvr2_krw_c PRIVATE src/app.c)
target_link_libraries(psvr2_krw_c PRIVATE psvr2krw_cli)
psvr2_enable_warnings(psvr2_krw_c)

if(PSVR2_BUILD_TOOLS)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)

    add_executable(psvr2_convert_ar30 tools/convert_ar30.c)
    target_link_libraries(psvr2_convert_ar30 PRIVATE psvr2krw)

    if(UNIX)
        add_executable(psvr2_serial_tool tools/serial_tool.c)
        target_link_libraries(psvr2_serial_tool PRIVATE psvr2krw)

        add_executable(psvr2_controller_bridge tools/controller_bridge.c)
        target_link_libraries(psvr2_controller_bridge PRIVATE psvr2krw)
    endif()

    if(SDL2_FOUND)
        add_executable(psvr2_stream_viewer tools/stream_viewer.c)
        target_link_libraries(psvr2_stream_viewer PRIVATE psvr2krw PkgConfig::SDL2)
    endif()
endif()

install(TARGETS psvr2_krw_c
    RUNTIME DESTINATION bin
)
if(PSVR2_BUILD_TOOLS)
    install(TARGETS psvr2_convert_ar30
        RUNTIME DESTINATION bin
    )
    if(TARGET psvr2_serial_tool)
        install(TARGETS psvr2_serial_tool RUNTIME DESTINATION bin)
    endif()
    if(TARGET psvr2_stream_viewer)
        install(TARGETS psvr2_stream_viewer RUNTIME DESTINATION bin)
    endif()
    if(TARGET psvr2_controller_bridge)
        install(TARGETS psvr2_controller_bridge RUNTIME DESTINATION bin)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(psvr2_unit_tests
        tests/test_core.c
        tests/test_maintenance.c
        tests/test_shell.c
        tests/test_stage1.c
    )
    target_include_directories(psvr2_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(
        psvr2_unit_tests PRIVATE psvr2krw_cli)
    target_compile_definitions(psvr2_unit_tests PRIVATE
        PSVR2_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    psvr2_enable_warnings(psvr2_unit_tests)
    target_compile_options(psvr2_unit_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>
        $<$<C_COMPILER_ID:MSVC>:/UNDEBUG>
    )
    add_test(NAME psvr2_unit_tests COMMAND psvr2_unit_tests)
    add_test(NAME psvr2_cli_help COMMAND psvr2_krw_c --help)
    add_test(NAME psvr2_cli_version COMMAND psvr2_krw_c --version)
    if(UNIX)
        add_executable(psvr2_line_editor_pty_driver tests/line_editor_pty_driver.c)
        target_include_directories(psvr2_line_editor_pty_driver PRIVATE src)
        target_link_libraries(psvr2_line_editor_pty_driver PRIVATE psvr2krw_cli)
        psvr2_enable_warnings(psvr2_line_editor_pty_driver)
        add_executable(psvr2_shell_completion_harness
            tests/shell_completion_harness.c src/shell_completion.c src/util.c)
        target_include_directories(psvr2_shell_completion_harness PRIVATE include src)
        target_link_libraries(psvr2_shell_completion_harness PRIVATE PkgConfig::LIBUSB)
        target_compile_options(psvr2_shell_completion_harness PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>)
        psvr2_enable_warnings(psvr2_shell_completion_harness)
        add_executable(psvr2_busybox_jobs_harness
            tests/busybox_jobs_harness.c src/shell_busybox.c src/util.c)
        target_include_directories(psvr2_busybox_jobs_harness PRIVATE include src)
        target_link_libraries(psvr2_busybox_jobs_harness PRIVATE PkgConfig::LIBUSB Threads::Threads)
        target_compile_definitions(psvr2_busybox_jobs_harness PRIVATE PSVR2_SHELL_JOB_WAIT_SECONDS=0.2)
        target_compile_options(psvr2_busybox_jobs_harness PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>)
        psvr2_enable_warnings(psvr2_busybox_jobs_harness)
        add_executable(psvr2_stage1_discovery_harness
            tests/stage1_discovery_harness.c src/stage1.c
            src/stage1_discovery.c src/util.c)
        target_include_directories(psvr2_stage1_discovery_harness PRIVATE include src)
        target_link_libraries(psvr2_stage1_discovery_harness PRIVATE PkgConfig::LIBUSB)
        target_compile_options(psvr2_stage1_discovery_harness PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>)
        psvr2_enable_warnings(psvr2_stage1_discovery_harness)
        add_test(NAME psvr2_stage1_discovery
            COMMAND psvr2_stage1_discovery_harness)
        add_executable(psvr2_bulk_prepare_harness
            tests/bulk_prepare_harness.c src/shell_bulk.c src/util.c)
        target_include_directories(psvr2_bulk_prepare_harness PRIVATE include src)
        target_link_libraries(psvr2_bulk_prepare_harness PRIVATE PkgConfig::LIBUSB)
        target_compile_options(psvr2_bulk_prepare_harness PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>)
        psvr2_enable_warnings(psvr2_bulk_prepare_harness)
        add_test(NAME psvr2_bulk_prepare COMMAND psvr2_bulk_prepare_harness)
        set_tests_properties(psvr2_bulk_prepare PROPERTIES TIMEOUT 10)
        add_executable(psvr2_vfs_cached_read_harness
            tests/vfs_cached_read_harness.c src/vfs.c src/util.c)
        target_include_directories(psvr2_vfs_cached_read_harness PRIVATE include src)
        target_link_libraries(psvr2_vfs_cached_read_harness PRIVATE PkgConfig::LIBUSB)
        target_compile_options(psvr2_vfs_cached_read_harness PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>)
        psvr2_enable_warnings(psvr2_vfs_cached_read_harness)
        add_test(NAME psvr2_vfs_cached_read COMMAND psvr2_vfs_cached_read_harness)
        add_executable(psvr2_persistence_durability_harness
            tests/persistence_durability_harness.c src/util.c)
        target_include_directories(psvr2_persistence_durability_harness PRIVATE include src)
        target_link_libraries(psvr2_persistence_durability_harness PRIVATE PkgConfig::LIBUSB)
        target_compile_options(psvr2_persistence_durability_harness PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>)
        psvr2_enable_warnings(psvr2_persistence_durability_harness)
        add_test(NAME psvr2_persistence_durability
            COMMAND psvr2_persistence_durability_harness)
        add_executable(psvr2_serial_reentry_harness
            tests/serial_reentry_harness.c src/util.c)
        target_include_directories(psvr2_serial_reentry_harness PRIVATE include src)
        target_link_libraries(psvr2_serial_reentry_harness PRIVATE PkgConfig::LIBUSB)
        target_compile_options(psvr2_serial_reentry_harness PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG;-ffunction-sections;-fdata-sections>)
        if(APPLE)
            target_link_options(psvr2_serial_reentry_harness PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(psvr2_serial_reentry_harness PRIVATE -Wl,--gc-sections)
        endif()
        psvr2_enable_warnings(psvr2_serial_reentry_harness)
        add_test(NAME psvr2_serial_reentry COMMAND psvr2_serial_reentry_harness)
        set_tests_properties(psvr2_serial_reentry PROPERTIES TIMEOUT 15)
        if(PSVR2_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
            target_compile_options(psvr2_shell_completion_harness PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(psvr2_shell_completion_harness PRIVATE -fsanitize=address,undefined)
            target_compile_options(psvr2_busybox_jobs_harness PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(psvr2_busybox_jobs_harness PRIVATE -fsanitize=address,undefined)
            target_compile_options(psvr2_stage1_discovery_harness PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(psvr2_stage1_discovery_harness PRIVATE -fsanitize=address,undefined)
            target_compile_options(psvr2_bulk_prepare_harness PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(psvr2_bulk_prepare_harness PRIVATE -fsanitize=address,undefined)
            target_compile_options(psvr2_vfs_cached_read_harness PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(psvr2_vfs_cached_read_harness PRIVATE -fsanitize=address,undefined)
            target_compile_options(psvr2_persistence_durability_harness PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(psvr2_persistence_durability_harness PRIVATE -fsanitize=address,undefined)
            target_compile_options(psvr2_serial_reentry_harness PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(psvr2_serial_reentry_harness PRIVATE -fsanitize=address,undefined)
        endif()
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME psvr2_shell_completion
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shell_completion.py
                    $<TARGET_FILE:psvr2_shell_completion_harness>)
            add_test(NAME psvr2_line_editor_pty
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_line_editor_pty.py
                    $<TARGET_FILE:psvr2_line_editor_pty_driver>)
            add_test(NAME psvr2_busybox_jobs
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_busybox_jobs.py
                    $<TARGET_FILE:psvr2_busybox_jobs_harness>)
        endif()
    endif()
    add_test(
        NAME psvr2_cli_kernel_probe_option_guard
        COMMAND psvr2_krw_c --kernel-probe --read-only
    )
    add_test(
        NAME psvr2_cli_kernel_probe_irrelevant_option_guard
        COMMAND psvr2_krw_c --kernel-probe --tmp
    )
    add_test(
        NAME psvr2_cli_kernel_probe_firmware_override_guard
        COMMAND psvr2_krw_c --kernel-probe --fw 06.00
    )
    add_test(
        NAME psvr2_cli_safety_audit_option_guard
        COMMAND psvr2_krw_c --safety-audit --read-only
    )
    add_test(
        NAME psvr2_cli_reboot_option_guard
        COMMAND psvr2_krw_c --reboot --read-only
    )
    set_tests_properties(
        psvr2_cli_kernel_probe_option_guard
        psvr2_cli_kernel_probe_irrelevant_option_guard
        psvr2_cli_kernel_probe_firmware_override_guard
        psvr2_cli_safety_audit_option_guard
        psvr2_cli_reboot_option_guard
        PROPERTIES WILL_FAIL TRUE)
    if(PSVR2_FIRMWARE_FIXTURES_DIR)
        add_test(
            NAME exact_fw0600_binary_regression
            COMMAND ${CMAKE_COMMAND}
                -DFIXTURE_ROOT=${PSVR2_FIRMWARE_FIXTURES_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_exact_images.cmake
        )
        add_test(
            NAME exact_fw0110_binary_regression
            COMMAND ${CMAKE_COMMAND}
                -DFIXTURE_ROOT=${PSVR2_FIRMWARE_FIXTURES_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_exact_0110.cmake
        )
    endif()
endif()
