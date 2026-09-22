add_executable(TaskListModelTest
  TaskListModelTest.cpp
  ${REPO_ROOT}/src/activities/tasks/TaskListModel.cpp
)

target_include_directories(TaskListModelTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/tasks
)

target_link_libraries(TaskListModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TaskListModelTest)
