# Host tests for the loader's platform independent logic.
#
# These tests do not need an Android device: they exercise the mountinfo reader
# and the zygote_next marker predicate, the two pieces that have to stay
# isomorphic with DuckDetector.
#
#   cmake -S loader/tests/host -B build/host-tests
#   cmake --build build/host-tests
#   ctest --test-dir build/host-tests --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(neozygisk_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(LOADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

add_executable(mount_marker_test
    mount_marker_test.cpp
    ${LOADER_SRC}/common/mount_marker.cpp
)
target_include_directories(mount_marker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LOADER_SRC}/include
)

add_executable(mount_info_test
    mount_info_test.cpp
    ${LOADER_SRC}/common/mount_info.cpp
)
target_include_directories(mount_info_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LOADER_SRC}/include
)

add_executable(mount_plan_test
    mount_plan_test.cpp
    ${LOADER_SRC}/common/mount_info.cpp
    ${LOADER_SRC}/common/mount_marker.cpp
    ${LOADER_SRC}/common/zygote_next_mount_plan.cpp
)
target_include_directories(mount_plan_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LOADER_SRC}/include
)

enable_testing()
add_test(NAME mount_marker_test COMMAND mount_marker_test)
add_test(NAME mount_info_test COMMAND mount_info_test)
add_test(NAME mount_plan_test COMMAND mount_plan_test)
