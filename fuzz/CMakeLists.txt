cmake_minimum_required(VERSION 3.16)
project(xcache_fuzz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(xcache_fuzz_lib
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/xcache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/xcache_c.cpp
)
target_include_directories(xcache_fuzz_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_include_directories(xcache_fuzz_lib SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/xxhash)
target_compile_features(xcache_fuzz_lib PUBLIC cxx_std_17)

add_executable(fuzz_corrupt fuzz_corrupt.cpp)
target_include_directories(fuzz_corrupt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(fuzz_corrupt xcache_fuzz_lib)

add_executable(fuzz_api fuzz_api.cpp)
target_include_directories(fuzz_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(fuzz_api xcache_fuzz_lib)

add_executable(fuzz_concurrent fuzz_concurrent.cpp)
target_include_directories(fuzz_concurrent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(fuzz_concurrent xcache_fuzz_lib Threads::Threads)
