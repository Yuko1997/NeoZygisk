cmake_minimum_required(VERSION 3.22.1)
project("loader")

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(WORK_DIRECTORY "/data/adb/neozygisk" CACHE STRING "NeoZygisk working directory")
option(NEOZYGISK_ZYGOTE_NEXT_TESTING "Enable debug-only zygote_next fault injection" OFF)
option(NEOZYGISK_PTRACE_INTERNAL_LOG "Enable debug-only ptrace file logging" OFF)

add_definitions(-DZKSU_VERSION=\"${ZKSU_VERSION}\")
add_definitions(-DWORK_DIRECTORY=\"${WORK_DIRECTORY}\")
if(NEOZYGISK_ZYGOTE_NEXT_TESTING)
    add_definitions(-DNEOZYGISK_ZYGOTE_NEXT_TESTING)
endif()

file(GLOB COMMON_SRC_LIST CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp")
add_library(common STATIC ${COMMON_SRC_LIST})
target_include_directories(common PRIVATE include)
target_link_libraries(common log)

aux_source_directory(injector INJECTOR_SRC_LIST)
add_library(zygisk SHARED ${INJECTOR_SRC_LIST})
target_compile_options(zygisk PRIVATE -Wno-invalid-offsetof -Wno-unused-private-field)
target_include_directories(zygisk PRIVATE include)
target_link_libraries(zygisk log common lsplt_static)

file(GLOB PTRACER_SRC_LIST CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ptracer/*.cpp")
add_executable(libzygisk_ptrace.so ${PTRACER_SRC_LIST})
target_include_directories(libzygisk_ptrace.so PRIVATE include)
target_link_libraries(libzygisk_ptrace.so log common)
if(NEOZYGISK_PTRACE_INTERNAL_LOG)
    target_compile_definitions(libzygisk_ptrace.so PRIVATE NEOZYGISK_PTRACE_INTERNAL_LOG)
endif()

add_subdirectory(external)
