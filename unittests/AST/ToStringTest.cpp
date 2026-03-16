// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <gtest/gtest.h>

#include "cangjie/AST/Node.h"

#include "cangjie/Parse/Parser.h"
#include "cangjie/Basic/DiagnosticEngine.h"

using namespace Cangjie;
using namespace AST;

namespace {
OwnedPtr<Expr> ParseExprFromSrc(const std::string& src)
{
    static DiagnosticEngine diag;
    static SourceManager sm;
    sm.AddSource("./", src);
    diag.SetSourceManager(&sm);
    Parser parser(src, diag, diag.GetSourceManager(), {});
    return parser.ParseExpr();
}

OwnedPtr<Type> ParseTypeFromSrc(const std::string& src)
{
    static DiagnosticEngine diag;
    static SourceManager sm;
    sm.AddSource("./", src);
    diag.SetSourceManager(&sm);
    Parser parser(src, diag, diag.GetSourceManager(), {});
    return parser.ParseType();
}

OwnedPtr<Decl> ParseDeclFromSrc(const std::string& src)
{
    static DiagnosticEngine diag;
    static SourceManager sm;
    sm.AddSource("./", src);
    diag.SetSourceManager(&sm);
    Parser parser(src, diag, diag.GetSourceManager(), {});
    return parser.ParseDecl(ScopeKind::TOPLEVEL);
}

OwnedPtr<File> ParseFileFromSrc(const std::string& src)
{
    static DiagnosticEngine diag;
    static SourceManager sm;
    sm.AddSource("./", src);
    diag.SetSourceManager(&sm);
    Parser parser(src, diag, diag.GetSourceManager(), {});
    return parser.ParseTopLevel();
}
} // namespace

TEST(ToStringTest, BasicNodeSourceRestore)
{
    auto expectExprToString = [](const std::string& src) {
        auto expr = ParseExprFromSrc(src);
        ASSERT_NE(expr, nullptr) << "Failed to parse: " << src;
        EXPECT_EQ(src, expr->ToString());
    };

    expectExprToString("a.b");
    expectExprToString("foo()");
    expectExprToString("foo(1)");
    expectExprToString("foo(1, 2)");
    expectExprToString("[1]");
    expectExprToString("[1, 2]");
}

TEST(ToStringTest, BasicNodeNullSafety)
{
    CallExpr callWithoutBase;
    auto arg = MakeOwned<FuncArg>();
    arg->expr = ParseExprFromSrc("1");
    callWithoutBase.args.emplace_back(std::move(arg));
    EXPECT_EQ("(1)", callWithoutBase.ToString());

    FuncArg argWithoutExpr;
    argWithoutExpr.name = SrcIdentifier("name");
    argWithoutExpr.colonPos = Position{0, 5, 5};
    EXPECT_EQ("name:", argWithoutExpr.ToString());

    MemberAccess memberWithoutBase;
    memberWithoutBase.field = SrcIdentifier("field");
    EXPECT_EQ("field", memberWithoutBase.ToString());

    ArrayExpr arrayWithoutType;
    auto arrayArg = MakeOwned<FuncArg>();
    arrayArg->expr = ParseExprFromSrc("2");
    arrayWithoutType.args.emplace_back(std::move(arrayArg));
    EXPECT_EQ("(2)", arrayWithoutType.ToString());

    EXPECT_NO_THROW({
        (void)callWithoutBase.ToString();
        (void)argWithoutExpr.ToString();
        (void)memberWithoutBase.ToString();
        (void)arrayWithoutType.ToString();
    });
}

TEST(ToStringTest, ExtendedNodesParsing)
{
    auto expectTypeToString = [](const std::string& src) {
        auto type = ParseTypeFromSrc(src);
        ASSERT_NE(type, nullptr) << "Failed to parse type: " << src;
        EXPECT_NO_THROW({ (void)type->ToString(); });
    };

    auto expectDeclToString = [](const std::string& src) {
        auto decl = ParseDeclFromSrc(src);
        ASSERT_NE(decl, nullptr) << "Failed to parse decl: " << src;
        EXPECT_NO_THROW({ (void)decl->ToString(); });
    };

    auto expectFileToString = [](const std::string& src) {
        auto file = ParseFileFromSrc(src);
        ASSERT_NE(file, nullptr) << "Failed to parse file: " << src;
        EXPECT_NO_THROW({ (void)file->ToString(); });
    };

    // Test parser coverage over RefType, VarDecl, MultiModifiers, etc.
    expectTypeToString("Int64");
    expectTypeToString("Array<Int64>");
    expectTypeToString("*Int64");

    expectDeclToString("let x = 1");
    expectDeclToString("var myVar: Int64 = 0");
    expectDeclToString("public mut var x: Int64 = 0");

    expectFileToString("package my_pkg\n");
    expectFileToString("import some_pkg.*\n");
    EXPECT_NO_THROW({ (void)ParseFileFromSrc("@C\nfunc c_foo() {}\n")->ToString(); });
}
