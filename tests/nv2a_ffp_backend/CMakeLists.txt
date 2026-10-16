cmake_minimum_required(VERSION 3.20)
project(nv2a_ffp_backend_regression C)
enable_testing()
add_executable(nv2a-ffp-backend-test nv2a_ffp_backend_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
target_include_directories(nv2a-ffp-backend-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-ffp-backend-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-ffp-backend-test PRIVATE m pthread)
endif()
add_test(NAME nv2a-ffp-backend COMMAND nv2a-ffp-backend-test)
# The executor reads an attribute with no array from its current value only when
# the title opts in (RECOMP_VERTEX_CURRENT_ATTRS); the test above checks both.
add_test(NAME nv2a-ffp-backend-current-attrs COMMAND nv2a-ffp-backend-test)
set_tests_properties(nv2a-ffp-backend-current-attrs PROPERTIES ENVIRONMENT "RECOMP_VERTEX_CURRENT_ATTRS=1")
