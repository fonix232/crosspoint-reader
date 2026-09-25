add_executable(BookOrbitProgressPolicyTest
  BookOrbitProgressPolicyTest.cpp
  BookOrbitSyncScheduleTest.cpp
  ReadingCardStateTest.cpp
)

target_include_directories(BookOrbitProgressPolicyTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitProgressPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitProgressPolicyTest)
