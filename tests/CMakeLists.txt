set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(../third_party/googletest googletest EXCLUDE_FROM_ALL)

file(GLOB TEST_SRCS CONFIGURE_DEPENDS test_*.cpp test_*.c)

foreach(TEST_SRC ${TEST_SRCS})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} PRIVATE xcache GTest::gtest GTest::gtest_main)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
