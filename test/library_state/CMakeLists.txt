add_executable(LibraryStateTest
  LibraryStateTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryStateCodec.cpp
)

target_include_directories(LibraryStateTest PRIVATE
  ../library_index_file/stubs
  ${REPO_ROOT}/lib/LibraryIndex
)

target_link_libraries(LibraryStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryStateTest)
