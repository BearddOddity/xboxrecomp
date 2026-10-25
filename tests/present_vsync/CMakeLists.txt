cmake_minimum_required(VERSION 3.20)
project(present_vsync_test C)
# RECOMP_PRESENT_VSYNC is read by both presenters through present_vsync_on():
# unset on, empty or 0 off, anything else on. Pure logic, no display needed.
set(CMAKE_C_STANDARD 11)
add_executable(present_vsync_test test_main.c)
target_include_directories(present_vsync_test PRIVATE ../../src/video)
enable_testing()
add_test(NAME present_vsync COMMAND present_vsync_test)
