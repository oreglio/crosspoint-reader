add_executable(TaskSyncReaderTest
  TaskSyncReaderTest.cpp
  ${REPO_ROOT}/src/tasks/TaskSyncReader.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(TaskSyncReaderTest PRIVATE
  ${REPO_ROOT}/src/tasks
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(TaskSyncReaderTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(TaskSyncReaderTest)
