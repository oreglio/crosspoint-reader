add_executable(TaskNotePagerTest
  TaskNotePagerTest.cpp
  ${REPO_ROOT}/src/activities/tasks/TaskNotePager.cpp
)

target_include_directories(TaskNotePagerTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/tasks
)

target_link_libraries(TaskNotePagerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TaskNotePagerTest)
