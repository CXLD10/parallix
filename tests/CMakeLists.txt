include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
# Windows/WSL note: forces the same CRT googletest links against; harmless on Linux.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Add each new test file below as you create it, e.g.:
#   test_lexer.cpp
#   test_parser.cpp
#   test_sema.cpp
#   test_lowering.cpp
#   test_dependence.cpp
add_executable(parallix_tests
  test_version.cpp
  test_lexer.cpp
  test_parser.cpp
  test_sema.cpp
  test_lowering.cpp
  test_dependence.cpp
)
target_link_libraries(parallix_tests PRIVATE parallix_core gtest_main)

include(GoogleTest)
# WORKING_DIRECTORY so tests can load examples/*.plx by their repo-relative path.
gtest_discover_tests(parallix_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
