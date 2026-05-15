cmake_minimum_required(VERSION 3.16)
project(xcache VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "tvOS" OR CMAKE_SYSTEM_NAME STREQUAL "watchOS" OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    option(XCACHE_BUILD_TESTS "Build tests" OFF)
    option(XCACHE_BUILD_BENCHMARKS "Build benchmarks" OFF)
    option(XCACHE_BUILD_EXAMPLES "Build examples" OFF)
else()
    option(XCACHE_BUILD_TESTS "Build tests" ON)
    option(XCACHE_BUILD_BENCHMARKS "Build benchmarks" ON)
    option(XCACHE_BUILD_EXAMPLES "Build examples" ON)

    option(XCACHE_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)
endif()
if(XCACHE_ENABLE_CLANG_TIDY)
        find_program(CLANG_TIDY clang-tidy)
        if(CLANG_TIDY)
            set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY}")
            message(STATUS "clang-tidy: enabled (${CLANG_TIDY})")
        else()
            message(WARNING "XCACHE_ENABLE_CLANG_TIDY=ON but clang-tidy not found")
        endif()
    endif()

find_package(Threads REQUIRED)

add_library(xcache
    src/xcache.cpp
    src/xcache_c.cpp
)
target_include_directories(xcache PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(xcache SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash>
)
target_compile_features(xcache PUBLIC cxx_std_17)
target_link_libraries(xcache PUBLIC Threads::Threads)

if(XCACHE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(XCACHE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(XCACHE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS xcache
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/xcache
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

set(XCACHE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/xcache")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/xcache-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/xcache-config.cmake"
    INSTALL_DESTINATION "${XCACHE_INSTALL_CMAKEDIR}"
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/xcache-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/xcache-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/xcache-config-version.cmake"
    DESTINATION "${XCACHE_INSTALL_CMAKEDIR}"
)
