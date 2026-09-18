# parallix_core holds all real compiler logic so tests/ can link against it
# directly instead of only being able to test through the CLI.
#
# Add each new .cpp file below as you create it, e.g.:
#   lexer/lexer.cpp
#   parser/parser.cpp
#   sema/sema.cpp
#   ir/lowering.cpp
#   dependence/dependence.cpp
add_library(parallix_core
  version.cpp
  lexer/lexer.cpp
  parser/parser.cpp
  sema/sema.cpp
  ir/lowering.cpp
  dependence/dependence.cpp
)
target_include_directories(parallix_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(parallix main.cpp)
target_link_libraries(parallix PRIVATE parallix_core)
