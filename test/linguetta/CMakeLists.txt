# Linguetta host tests. Fixtures in fixtures/ are copied verbatim from the server
# repo (docs/sync/fixtures) and must not be edited here.
#
# Built as part of test/CMakeLists.txt, or on its own (no zlib/expat needed):
#   cmake -S test/linguetta -B build/linguetta && cmake --build build/linguetta && ctest --test-dir build/linguetta

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(linguetta_tests C CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  include(FetchContent)
  FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  add_library(crosspoint_test_common INTERFACE)
  target_include_directories(crosspoint_test_common INTERFACE ${REPO_ROOT} ${REPO_ROOT}/lib)
  target_compile_options(crosspoint_test_common INTERFACE -Wall -Wextra -pedantic)
  enable_testing()
  include(GoogleTest)
endif()

include(FetchContent)
FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

set(LINGUETTA_SRC ${REPO_ROOT}/src/linguetta)

file(GLOB LINGUETTA_CORE_SRC ${LINGUETTA_SRC}/core/*.cpp)
add_library(linguetta_core STATIC ${LINGUETTA_CORE_SRC} StdFs.cpp)
target_include_directories(linguetta_core PUBLIC ${arduinojson_SOURCE_DIR}/src ${LINGUETTA_SRC}/core ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(linguetta_core PUBLIC ARDUINOJSON_ENABLE_ARDUINO_STRING=0)
target_link_libraries(linguetta_core PUBLIC crosspoint_test_common)

target_compile_definitions(linguetta_core PUBLIC LINGUETTA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

foreach(name Fsrs Store Blend Decks Sync WordLens Queue DeckImport)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${name}Test.cpp)
    continue()
  endif()
  add_executable(Linguetta${name}Test ${name}Test.cpp)
  target_link_libraries(Linguetta${name}Test PRIVATE linguetta_core GTest::gtest_main)
  gtest_discover_tests(Linguetta${name}Test TEST_PREFIX "Linguetta.")
endforeach()

# KOSync partial MD5: upstream lib/KOReaderSync/KOReaderDocumentId.cpp, unchanged,
# against the in-memory HalStorage fake and a host MD5Builder.
add_executable(LinguettaKoHashTest KoHashTest.cpp ${REPO_ROOT}/lib/KOReaderSync/KOReaderDocumentId.cpp)
target_include_directories(LinguettaKoHashTest PRIVATE stubs ../library_builder/stubs ${REPO_ROOT}/lib/KOReaderSync)
target_link_libraries(LinguettaKoHashTest PRIVATE linguetta_core GTest::gtest_main)
gtest_discover_tests(LinguettaKoHashTest TEST_PREFIX "Linguetta.")
