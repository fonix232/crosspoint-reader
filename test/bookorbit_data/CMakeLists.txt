find_path(ARDUINOJSON_INCLUDE_DIR ArduinoJson.h
  HINTS ${REPO_ROOT}/.pio/libdeps/papers3/ArduinoJson/src
        ${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src
        ${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src)
if(NOT ARDUINOJSON_INCLUDE_DIR)
  FetchContent_Declare(arduinojson
    URL https://github.com/bblanchon/ArduinoJson/archive/refs/tags/v7.4.2.tar.gz)
  FetchContent_MakeAvailable(arduinojson)
  set(ARDUINOJSON_INCLUDE_DIR ${arduinojson_SOURCE_DIR}/src)
endif()
add_library(BookOrbitDataProtocol STATIC
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDataProtocol.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCatalogParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCatalogFormat.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(BookOrbitDataProtocol PUBLIC
  ${ARDUINOJSON_INCLUDE_DIR} ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include ${REPO_ROOT}/lib/LibraryIndex ${REPO_ROOT}/lib/Utf8)
add_executable(BookOrbitDataTest BookOrbitDataTest.cpp)
target_link_libraries(BookOrbitDataTest PRIVATE BookOrbitDataProtocol crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookOrbitDataTest)
add_executable(BookOrbitProtocolProbe BookOrbitProtocolProbe.cpp)
target_link_libraries(BookOrbitProtocolProbe PRIVATE BookOrbitDataProtocol)
add_executable(BookOrbitDataStoreTest BookOrbitDataStoreTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDataStore.cpp)
target_include_directories(BookOrbitDataStoreTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_link_libraries(BookOrbitDataStoreTest PRIVATE BookOrbitDataProtocol crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookOrbitDataStoreTest)

add_executable(BookOrbitReadingTrackerTest BookOrbitReadingTrackerTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitReadingTracker.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClock.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDataStore.cpp)
target_include_directories(BookOrbitReadingTrackerTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_link_libraries(BookOrbitReadingTrackerTest PRIVATE BookOrbitDataProtocol crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookOrbitReadingTrackerTest)

add_executable(BookOrbitFileStoreTest BookOrbitFileStoreTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitFileStore.cpp)
target_include_directories(BookOrbitFileStoreTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/lib/Memory)
target_link_libraries(BookOrbitFileStoreTest PRIVATE BookOrbitDataProtocol crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookOrbitFileStoreTest)
