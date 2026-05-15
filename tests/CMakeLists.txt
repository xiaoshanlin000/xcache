set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(../third_party/googletest googletest EXCLUDE_FROM_ALL)

add_executable(test_xcache test_xcache.cpp)
target_link_libraries(test_xcache PRIVATE xcache GTest::gtest GTest::gtest_main)
add_test(NAME test_xcache COMMAND test_xcache)

add_executable(test_xc test_xc.c)
target_link_libraries(test_xc PRIVATE xcache)
add_test(NAME test_xc COMMAND test_xc)
