cmake_minimum_required(VERSION 3.20)
project(parallix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()

option(PARALLIX_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(PARALLIX_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

option(PARALLIX_BUILD_TESTS "Build the unit and integration tests (needs GoogleTest)" ON)

add_subdirectory(src)

install(TARGETS parallix RUNTIME DESTINATION bin)
install(PROGRAMS demo.sh DESTINATION share/parallix)
install(DIRECTORY examples DESTINATION share/parallix)

if(PARALLIX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
