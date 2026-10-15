#include "parallix/pipeline.h"

#include <fstream>
#include <sstream>

#include "parallix/lexer.h"
#include "parallix/lowering.h"
#include "parallix/parser.h"
#include "parallix/sha256.h"
#include "parallix/sema.h"

namespace parallix {

bool ParseSourceFile(const std::string& path, ParsedSource* out, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    *error = "error: could not open '" + path + "'";
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  out->text = buf.str();
  try {
    Lexer lexer(out->text);
    out->tokens = lexer.Tokenize();
    Parser parser(out->tokens);
    out->program = parser.ParseProgram();
  } catch (const LexError& e) {
    *error = std::string("lex error: ") + e.what();
    return false;
  } catch (const ParseError& e) {
    *error = std::string("parse error: ") + e.what();
    return false;
  }
  return true;
}

bool AnalyzeKernelDecl(KernelDecl kernel, Analysis* out, std::string* error) {
  try {
    Sema sema;
    sema.AnalyzeKernel(kernel);
    out->pir = LowerKernel(kernel);
  } catch (const SemaError& e) {
    *error = e.what();
    return false;
  } catch (const LoweringError& e) {
    *error = std::string("lowering error: ") + e.what();
    return false;
  }
  out->kernel = std::move(kernel);
  out->report = AnalyzeDependence(out->pir);
  out->schedule = GenerateSchedule(out->pir, out->report);
  out->gen = GenerateCCode(out->kernel, out->pir, out->schedule);
  return true;
}

CodegenResult GenerateWith(const Analysis& analysis, const Schedule& schedule) {
  return GenerateCCode(analysis.kernel, analysis.pir, schedule);
}

std::string SourceHash(const std::string& text) { return Sha256Hex(text); }

}  // namespace parallix
