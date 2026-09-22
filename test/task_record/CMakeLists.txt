add_executable(TaskRecordTest TaskRecordTest.cpp)
target_include_directories(TaskRecordTest PRIVATE ${REPO_ROOT}/src/tasks)
target_link_libraries(TaskRecordTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TaskRecordTest)
