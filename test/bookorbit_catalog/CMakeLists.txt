add_executable(BookOrbitCatalogTest
  BookOrbitCatalogTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCatalogFormat.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCatalogParser.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BookOrbitCatalogTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(BookOrbitCatalogTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitCatalogTest)
