cmake_minimum_required(VERSION 3.25)
project(itch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ITCH_BUILD_TESTS "Build unit tests" ON)
option(ITCH_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(ITCH_SANITIZE)
  # -fno-sanitize-recover makes UBSan abort (and fail the test) instead of
  # printing a warning and carrying on.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Strict warnings for our own targets only (not third-party code).
add_library(itch_warnings INTERFACE)
target_compile_options(itch_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)

add_library(itch
  src/decoder.cpp
  src/frame_reader.cpp
  src/market.cpp
  src/order_book.cpp
)
target_include_directories(itch PUBLIC include)
target_link_libraries(itch PRIVATE itch_warnings)

add_executable(itch_stats tools/itch_stats.cpp)
target_link_libraries(itch_stats PRIVATE itch itch_warnings)

add_executable(itch_book tools/itch_book.cpp)
target_link_libraries(itch_book PRIVATE itch itch_warnings)

if(ITCH_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Treat GoogleTest headers as system headers so our -Werror flags don't
    # trip on warnings inside its templates (GCC and Clang warn differently).
    SYSTEM
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(itch_tests
    tests/test_decoder.cpp
    tests/test_frame_reader.cpp
    tests/test_order_book.cpp
  )
  target_link_libraries(itch_tests PRIVATE itch itch_warnings GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(itch_tests)
endif()
