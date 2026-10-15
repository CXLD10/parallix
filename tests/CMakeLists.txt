# Prefer a system GoogleTest (e.g. apt install libgtest-dev); otherwise fetch it.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  set(PARALLIX_GTEST_MAIN gtest_main)
else()
  set(PARALLIX_GTEST_MAIN GTest::gtest_main)
endif()

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
  test_schedule.cpp
  test_codegen.cpp
  test_harness.cpp
  test_dsl_extensions.cpp
  test_refutation.cpp
  test_certificate.cpp
  test_cost_model.cpp
  test_integration.cpp
)
target_link_libraries(parallix_tests PRIVATE parallix_core ${PARALLIX_GTEST_MAIN})

include(GoogleTest)
# WORKING_DIRECTORY so tests can load examples/*.plx by their repo-relative path.
gtest_discover_tests(parallix_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
