cmake_minimum_required(VERSION 3.20)
project(kernel_thread_suspended_test C)
# Windows only: PsCreateSystemThreadEx spawns a Win32 thread.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_thread_suspended_test test_main.c)
target_include_directories(kernel_thread_suspended_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_thread_suspended_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: RECOMP_HONOR_SUSPEND is read on each create, but the
# modes are kept apart like the other kernel tests.
add_test(NAME kernel_thread_suspended_default COMMAND kernel_thread_suspended_test default ${XBE})
add_test(NAME kernel_thread_suspended_honor COMMAND kernel_thread_suspended_test honor ${XBE})
