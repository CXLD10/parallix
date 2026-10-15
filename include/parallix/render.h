#pragma once

#include <string>
#include <vector>

#include "parallix/ast.h"
#include "parallix/ir.h"
#include "parallix/token.h"

namespace parallix {

// Human-readable renderings shared by the CLI, explanations and certificates.
std::string RenderTokens(const std::vector<Token>& tokens);
std::string RenderExpr(const Expr& expr);
std::string RenderType(const TypeNode& type);
std::string RenderStmt(const Stmt& stmt);
std::string RenderKernel(const KernelDecl& kernel);
std::string RenderAccessFunction(const AccessFunction& access);
std::string RenderReference(const Reference& ref);

}  // namespace parallix
