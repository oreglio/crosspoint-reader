add_executable(LibraryFavoritesTest
  LibraryFavoritesTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryFavorites.cpp
)

target_include_directories(LibraryFavoritesTest PRIVATE
  ${REPO_ROOT}/lib/LibraryIndex
)

target_link_libraries(LibraryFavoritesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryFavoritesTest)
