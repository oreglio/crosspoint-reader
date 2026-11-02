add_executable(TaskOpQueueTest
  TaskOpQueueTest.cpp
  ${REPO_ROOT}/src/tasks/TaskOpQueue.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)

target_include_directories(TaskOpQueueTest PRIVATE
  ${REPO_ROOT}/src/tasks
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)

target_link_libraries(TaskOpQueueTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TaskOpQueueTest)
