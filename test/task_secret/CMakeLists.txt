add_executable(TaskSecretTest
  TaskSecretTest.cpp
  ${REPO_ROOT}/src/tasks/TaskSecret.cpp
)

target_include_directories(TaskSecretTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TaskSecretTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TaskSecretTest)
