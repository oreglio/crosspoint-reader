add_executable(TaskOpQueueTest
  TaskOpQueueTest.cpp
  ${REPO_ROOT}/src/tasks/TaskOpQueue.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(TaskOpQueueTest PRIVATE
  ${REPO_ROOT}/src/tasks
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(TaskOpQueueTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TaskOpQueueTest)
