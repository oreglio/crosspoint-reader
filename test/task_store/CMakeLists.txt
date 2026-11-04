add_executable(TaskStoreTest
  TaskStoreTest.cpp
  ${REPO_ROOT}/src/TaskStore.cpp
  ${REPO_ROOT}/src/tasks/TaskOpQueue.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
)
target_include_directories(TaskStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/persistable_store_atomic/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/tasks
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)
target_link_libraries(TaskStoreTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(TaskStoreTest)
