add_executable(CountdownTest
  CountdownClockTest.cpp
  ${REPO_ROOT}/src/activities/util/CountdownClock.cpp
)

target_include_directories(CountdownTest PRIVATE
  ${REPO_ROOT}/src/activities/util
)

target_link_libraries(CountdownTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CountdownTest)
