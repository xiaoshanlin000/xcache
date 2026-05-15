set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(HAVE_LIB_RT OFF CACHE BOOL "" FORCE)
add_subdirectory(../third_party/benchmark benchmark EXCLUDE_FROM_ALL)

add_executable(bench_latency bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE xcache benchmark::benchmark Threads::Threads)
