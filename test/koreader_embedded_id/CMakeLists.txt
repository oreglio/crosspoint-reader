add_executable(KOReaderEmbeddedIdTest
  KOReaderEmbeddedIdTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderEmbeddedId.cpp
)

target_include_directories(KOReaderEmbeddedIdTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderEmbeddedIdTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(KOReaderEmbeddedIdTest)
