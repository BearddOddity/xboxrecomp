# Every test folder, in one tree. Used only with -DXBOXRECOMP_BUILD_TESTS=ON at
# the top level; each folder's own CMakeLists builds the SDK itself when it is
# configured alone, and skips itself (instead of failing) on the wrong OS here.
#
# Left out: folders without a CMakeLists, and d3d8_smoke, which the SDK's own
# D3D build already adds (src/d3d/CMakeLists.txt).
file(GLOB XBOXRECOMP_TEST_DIRS LIST_DIRECTORIES true RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT XBOXRECOMP_TEST_DIRS)
foreach(d IN LISTS XBOXRECOMP_TEST_DIRS)
    if(d STREQUAL "d3d8_smoke")
        continue()
    endif()
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${d}
       AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${d}/CMakeLists.txt)
        add_subdirectory(${d})
    endif()
endforeach()
