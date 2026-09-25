find_path(ARDUINOJSON_INCLUDE_DIR ArduinoJson.h
  HINTS ${REPO_ROOT}/.pio/libdeps/papers3/ArduinoJson/src
        ${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src)
if(NOT ARDUINOJSON_INCLUDE_DIR)
  FetchContent_Declare(arduinojson
    URL https://github.com/bblanchon/ArduinoJson/archive/refs/tags/v7.4.2.tar.gz)
  FetchContent_MakeAvailable(arduinojson)
  set(ARDUINOJSON_INCLUDE_DIR ${arduinojson_SOURCE_DIR}/src)
endif()
add_executable(BookOrbitInsightsTest
  BookOrbitInsightsTest.cpp
  BookOrbitInsightsCacheTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitInsightsCache.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitInsightsApi.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCatalogFormat.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitInsights.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BookOrbitInsightsTest PRIVATE
  ${REPO_ROOT}/test/bookorbit_data/stubs
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/BookOrbit
  ${ARDUINOJSON_INCLUDE_DIR}
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(BookOrbitInsightsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitInsightsTest)
