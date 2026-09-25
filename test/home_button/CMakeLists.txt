add_executable(HomeButtonInputTest HomeButtonInputTest.cpp ResetSleepPolicyTest.cpp)

target_include_directories(HomeButtonInputTest PRIVATE ../../src)

target_link_libraries(HomeButtonInputTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeButtonInputTest)
