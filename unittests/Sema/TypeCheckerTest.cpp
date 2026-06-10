// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include "gtest/gtest.h"
#include "TestCompilerInstance.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/PrintNode.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/Basic/Match.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
extern char **environ;
#endif

using namespace Cangjie;
using namespace AST;

namespace {
std::unordered_map<std::string, std::string> GetEnvironmentVars()
{
    std::unordered_map<std::string, std::string> envVars;
#ifdef _WIN32
    char **env = _environ;
#else
    char **env = environ;
#endif
    while (env && *env) {
        std::string entry(*env);
        size_t pos = entry.find('=');
        if (pos != std::string::npos) {
            std::string key = entry.substr(0, pos);
            std::string value = entry.substr(pos + 1);
            envVars[key] = value;
        }
        ++env;
    }
    return envVars;
}

VarDecl* FindVarDeclByName(File& file, const std::string& name)
{
    VarDecl* result = nullptr;
    Walker walker(&file, [&result, &name](Ptr<Node> node) -> VisitAction {
        if (auto vd = DynamicCast<VarDecl*>(node); vd && vd->identifier.Val() == name) {
            if (vd->initializer) {
                result = vd;
                return VisitAction::STOP_NOW;
            }
            if (!result) {
                result = vd;
            }
        }
        return VisitAction::WALK_CHILDREN;
    });
    walker.Walk();
    return result;
}

CallExpr* RequireDesugaredCall(Expr& expr)
{
    if (!expr.desugarExpr) {
        return nullptr;
    }
    if (expr.desugarExpr->astKind == ASTKind::CALL_EXPR) {
        return StaticAs<ASTKind::CALL_EXPR>(expr.desugarExpr.get());
    }
    return RequireDesugaredCall(*expr.desugarExpr);
}
}

class TypeCheckerTest : public testing::Test {
protected:
    void SetUp() override
    {
        instance = std::make_unique<TestCompilerInstance>(invocation, diag);
#ifdef PROJECT_SOURCE_DIR
        // Gets the absolute path of the project from the compile parameter.
        projectPath = PROJECT_SOURCE_DIR;
#else
        // Just in case, give it a default value.
        // Assume the initial is in the build directory.
        projectPath = "..";
#endif
#ifdef __x86_64__
        instance->invocation.globalOptions.target.arch = Cangjie::Triple::ArchType::X86_64;
#else
        instance->invocation.globalOptions.target.arch = Cangjie::Triple::ArchType::AARCH64;
#endif
#ifdef _WIN32
        instance->invocation.globalOptions.target.os = Cangjie::Triple::OSType::WINDOWS;
        invocation.globalOptions.executablePath = projectPath + "\\output\\bin\\";
#elif defined(__APPLE__)
        instance->invocation.globalOptions.target.os = Cangjie::Triple::OSType::DARWIN;
        invocation.globalOptions.executablePath = projectPath + "/output/bin/";
#elif defined(__unix__)
        instance->invocation.globalOptions.target.os = Cangjie::Triple::OSType::LINUX;
        invocation.globalOptions.executablePath = projectPath + "/output/bin/";
#endif
        std::string cangjieHome = projectPath + "/output";
#if defined(_WIN32)
        std::string platform = "windows_x86_64";
#elif defined(__APPLE__) && defined(__x86_64__)
        std::string platform = "darwin_x86_64";
#elif defined(__APPLE__)
        std::string platform = "darwin_aarch64";
#elif defined(__x86_64__)
        std::string platform = "linux_x86_64";
#else
        std::string platform = "linux_aarch64";
#endif
        std::string cangjiePath = cangjieHome + "/modules/" + platform + "_cjnative";
#ifdef _WIN32
        char* oldHome = getenv("CANGJIE_HOME");
        char* oldPath = getenv("CANGJIE_PATH");
        if (oldHome) savedCangjieHome = oldHome;
        if (oldPath) savedCangjiePath = oldPath;
        _putenv_s("CANGJIE_HOME", cangjieHome.c_str());
        _putenv_s("CANGJIE_PATH", cangjiePath.c_str());
#else
        char* oldHome = getenv("CANGJIE_HOME");
        char* oldPath = getenv("CANGJIE_PATH");
        if (oldHome) savedCangjieHome = oldHome;
        if (oldPath) savedCangjiePath = oldPath;
        setenv("CANGJIE_HOME", cangjieHome.c_str(), 1);
        setenv("CANGJIE_PATH", cangjiePath.c_str(), 1);
#endif
        invocation.globalOptions.ReadPathsFromEnvironmentVars(GetEnvironmentVars());
        Cangjie::MacroProcMsger::GetInstance().CloseMacroSrv();
    }

    void TearDown() override
    {
#ifdef _WIN32
        if (savedCangjieHome.empty()) {
            _putenv_s("CANGJIE_HOME", "");
        } else {
            _putenv_s("CANGJIE_HOME", savedCangjieHome.c_str());
        }
        if (savedCangjiePath.empty()) {
            _putenv_s("CANGJIE_PATH", "");
        } else {
            _putenv_s("CANGJIE_PATH", savedCangjiePath.c_str());
        }
#else
        if (savedCangjieHome.empty()) {
            unsetenv("CANGJIE_HOME");
        } else {
            setenv("CANGJIE_HOME", savedCangjieHome.c_str(), 1);
        }
        if (savedCangjiePath.empty()) {
            unsetenv("CANGJIE_PATH");
        } else {
            setenv("CANGJIE_PATH", savedCangjiePath.c_str(), 1);
        }
#endif
    }

    std::string srcPath;
    std::string projectPath;
    std::string savedCangjieHome;
    std::string savedCangjiePath;
    DiagnosticEngine diag;
    CompilerInvocation invocation;
    std::unique_ptr<TestCompilerInstance> instance;
};

TEST_F(TypeCheckerTest, TypecheckTest)
{
    std::string code = R"(
main() {
    var t: Int32
    t = 1
    return 0
}
    )";

    instance->code = code;
    instance->Compile(CompileStage::SEMA);

    Walker walker(instance->GetSourcePackages()[0]->files[0].get(), [](Ptr<Node> node) -> VisitAction {
        Meta::match (*node)(
            [](const FuncDecl& decl) {
                auto ty = dynamic_cast<FuncTy*>(decl.GetTy().get());
                EXPECT_EQ(ty->retTy->kind, TypeKind::TYPE_INT64);
            },
            [](const VarDecl& decl) { EXPECT_EQ(decl.TyKind(), TypeKind::TYPE_INT32); });
        return VisitAction::WALK_CHILDREN;
    });
    walker.Walk();
    EXPECT_EQ(diag.GetErrorCount(), 0);
}

TEST_F(TypeCheckerTest, ForcedCastDirectDesugarsToFromExtern)
{
    std::string code = R"(
public interface Runtime<T> where T <: Runtime<T> {
    static func memberAccess(e: Extern<T>, field: String): Extern<T>
    static func indexAccess(e: Extern<T>, arg: Any): Extern<T>
    static func memberUpdate(e: Extern<T>, field: String, value: Any): Unit
    static func indexUpdate(e: Extern<T>, field: Any, value: Any): Unit
    static func functionCall(e: Extern<T>, args: Array<Any>): Extern<T>
    static func fromExtern<R>(h: Extern<T>): R
    static func toExtern<R>(v: R): Extern<T>
}

public struct Extern<T> where T <: Runtime<T> {
    Extern(public let content: Any) {}
}

public class foo {}
public class DummyRuntime <: Runtime<DummyRuntime> {
    public static func memberAccess(e: Extern<DummyRuntime>, field: String): Extern<DummyRuntime> {
        return e
    }

    public static func indexAccess(e: Extern<DummyRuntime>, arg: Any): Extern<DummyRuntime> {
        return e
    }

    public static func memberUpdate(e: Extern<DummyRuntime>, field: String, value: Any): Unit {
    }

    public static func indexUpdate(e: Extern<DummyRuntime>, field: Any, value: Any): Unit {
    }

    public static func functionCall(e: Extern<DummyRuntime>, args: Array<Any>): Extern<DummyRuntime> {
        return e
    }

    public static func fromExtern<R>(h: Extern<DummyRuntime>): R {
        throw Exception()
    }

    public static func toExtern<R>(v: R): Extern<DummyRuntime> {
        return Extern<DummyRuntime>(v)
    }
}

func makeExtern(): Extern<DummyRuntime> {
    return Extern<DummyRuntime>(0)
}

main() {
    let externValue = makeExtern()
    let value = (foo)externValue
}
)";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    ASSERT_TRUE(instance->Compile(CompileStage::SEMA));
    ASSERT_EQ(diag.GetErrorCount(), 0);

    auto* valueDecl = FindVarDeclByName(*instance->GetSourcePackages()[0]->files[0], "value");
    ASSERT_NE(valueDecl, nullptr);
    ASSERT_NE(valueDecl->initializer, nullptr);
    EXPECT_EQ(valueDecl->initializer->astKind, ASTKind::FORCED_CAST_EXPR);
    auto* callExpr = RequireDesugaredCall(*valueDecl->initializer);
    ASSERT_NE(callExpr, nullptr);
    ASSERT_NE(callExpr->resolvedFunction, nullptr);
    EXPECT_EQ(callExpr->resolvedFunction->identifier.Val(), "fromExtern");
    auto* memberAccess = DynamicCast<MemberAccess*>(callExpr->baseFunc.get());
    ASSERT_NE(memberAccess, nullptr);
    EXPECT_EQ(memberAccess->field.Val(), "fromExtern");
    ASSERT_EQ(memberAccess->typeArguments.size(), 1);
    EXPECT_EQ(memberAccess->typeArguments[0]->ToString(), "foo");
    EXPECT_EQ(valueDecl->GetTy()->String(), "Class-foo");
}

TEST_F(TypeCheckerTest, ForcedCastGenericTargetDesugarsToFromExtern)
{
    std::string code = R"(
public interface Runtime<T> where T <: Runtime<T> {
    static func memberAccess(e: Extern<T>, field: String): Extern<T>
    static func indexAccess(e: Extern<T>, arg: Any): Extern<T>
    static func memberUpdate(e: Extern<T>, field: String, value: Any): Unit
    static func indexUpdate(e: Extern<T>, field: Any, value: Any): Unit
    static func functionCall(e: Extern<T>, args: Array<Any>): Extern<T>
    static func fromExtern<R>(h: Extern<T>): R
    static func toExtern<R>(v: R): Extern<T>
}

public struct Extern<T> where T <: Runtime<T> {
    Extern(public let content: Any) {}
}

public class Box<T> {}
public class DummyRuntime <: Runtime<DummyRuntime> {
    public static func memberAccess(e: Extern<DummyRuntime>, field: String): Extern<DummyRuntime> {
        return e
    }

    public static func indexAccess(e: Extern<DummyRuntime>, arg: Any): Extern<DummyRuntime> {
        return e
    }

    public static func memberUpdate(e: Extern<DummyRuntime>, field: String, value: Any): Unit {
    }

    public static func indexUpdate(e: Extern<DummyRuntime>, field: Any, value: Any): Unit {
    }

    public static func functionCall(e: Extern<DummyRuntime>, args: Array<Any>): Extern<DummyRuntime> {
        return e
    }

    public static func fromExtern<R>(h: Extern<DummyRuntime>): R {
        throw Exception()
    }

    public static func toExtern<R>(v: R): Extern<DummyRuntime> {
        return Extern<DummyRuntime>(v)
    }
}

func makeExtern(): Extern<DummyRuntime> {
    return Extern<DummyRuntime>(0)
}

main() {
    let externValue = makeExtern()
    let value = (Box<Int64>)externValue
}
)";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    ASSERT_TRUE(instance->Compile(CompileStage::SEMA));
    ASSERT_EQ(diag.GetErrorCount(), 0);

    auto* valueDecl = FindVarDeclByName(*instance->GetSourcePackages()[0]->files[0], "value");
    ASSERT_NE(valueDecl, nullptr);
    ASSERT_NE(valueDecl->initializer, nullptr);
    EXPECT_EQ(valueDecl->initializer->astKind, ASTKind::FORCED_CAST_EXPR);
    auto* callExpr = RequireDesugaredCall(*valueDecl->initializer);
    ASSERT_NE(callExpr, nullptr);
    ASSERT_NE(callExpr->resolvedFunction, nullptr);
    EXPECT_EQ(callExpr->resolvedFunction->identifier.Val(), "fromExtern");
    auto* memberAccess = DynamicCast<MemberAccess*>(callExpr->baseFunc.get());
    ASSERT_NE(memberAccess, nullptr);
    EXPECT_EQ(memberAccess->field.Val(), "fromExtern");
    ASSERT_EQ(memberAccess->typeArguments.size(), 1);
    EXPECT_EQ(memberAccess->typeArguments[0]->ToString(), "Box<Int64>");
    EXPECT_EQ(valueDecl->GetTy()->String(), "Class-Box<Int64>");
}

TEST_F(TypeCheckerTest, ForcedCastDirectNonExternOperandReportsInteropDiagnostic)
{
    std::string code = R"(
public class foo {}

main() {
    let value: foo = (foo)1
}
)";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    EXPECT_FALSE(instance->Compile(CompileStage::SEMA));

    auto diags = diag.GetCategoryDiagnostic(DiagCategory::SEMA);
    bool foundForcedCastDiag = false;
    for (auto& d : diags) {
        auto message = d.GetErrorMessage();
        if (message.find("invalid interoperation forced cast") != std::string::npos) {
            foundForcedCastDiag = true;
            EXPECT_NE(message.find("'(U)e' requires 'U' to be a type"), std::string::npos);
            EXPECT_NE(message.find("'Extern' type"), std::string::npos);
            EXPECT_NE(message.find("use 'as' for ordinary type conversions"), std::string::npos);
        }
    }
    EXPECT_TRUE(foundForcedCastDiag);
}

TEST_F(TypeCheckerTest, ForcedCastAmbiguousFallbackKeepsOriginalCallPath)
{
    std::string code = R"(
func foo(x: Int64): Int64 {
    return x + 1
}

main() {
    let value = (foo)(1)
}
)";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    ASSERT_TRUE(instance->Compile(CompileStage::SEMA));
    ASSERT_EQ(diag.GetErrorCount(), 0);

    auto* valueDecl = FindVarDeclByName(*instance->GetSourcePackages()[0]->files[0], "value");
    ASSERT_NE(valueDecl, nullptr);
    ASSERT_NE(valueDecl->initializer, nullptr);
    EXPECT_EQ(valueDecl->initializer->astKind, ASTKind::AMBIGUOUS_FORCED_CAST_EXPR);
    auto* callExpr = RequireDesugaredCall(*valueDecl->initializer);
    ASSERT_NE(callExpr, nullptr);
    EXPECT_EQ(callExpr->GetTy()->String(), "Int64");
    auto* parenExpr = DynamicCast<ParenExpr*>(callExpr->baseFunc.get());
    ASSERT_NE(parenExpr, nullptr);
    auto* refExpr = DynamicCast<RefExpr*>(parenExpr->expr.get());
    ASSERT_NE(refExpr, nullptr);
    EXPECT_EQ(refExpr->ref.identifier.Val(), "foo");
}

TEST_F(TypeCheckerTest, ForcedCastAmbiguousFallbackKeepsOriginalBinaryPath)
{
    std::string code = R"(
main() {
    let foo: Int64 = 2
    let value = (foo)-1
}
)";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    ASSERT_TRUE(instance->Compile(CompileStage::SEMA));
    ASSERT_EQ(diag.GetErrorCount(), 0);

    auto* valueDecl = FindVarDeclByName(*instance->GetSourcePackages()[0]->files[0], "value");
    ASSERT_NE(valueDecl, nullptr);
    ASSERT_NE(valueDecl->initializer, nullptr);
    EXPECT_EQ(valueDecl->initializer->astKind, ASTKind::AMBIGUOUS_FORCED_CAST_EXPR);
    ASSERT_NE(valueDecl->initializer->desugarExpr, nullptr);
    auto* binaryExpr = DynamicCast<BinaryExpr*>(valueDecl->initializer->desugarExpr.get());
    ASSERT_NE(binaryExpr, nullptr);
    EXPECT_EQ(binaryExpr->op, TokenKind::SUB);
    EXPECT_EQ(binaryExpr->GetTy()->String(), "Int64");
}

TEST_F(TypeCheckerTest, ForcedCastAmbiguousFallbackKeepsOriginalExtendedBinaryPath)
{
    std::string code = R"(
main() {
    let foo: Int64 = 2
    let value = (foo)-1 * 2
}
)";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    ASSERT_TRUE(instance->Compile(CompileStage::SEMA));
    ASSERT_EQ(diag.GetErrorCount(), 0);

    auto* valueDecl = FindVarDeclByName(*instance->GetSourcePackages()[0]->files[0], "value");
    ASSERT_NE(valueDecl, nullptr);
    ASSERT_NE(valueDecl->initializer, nullptr);
    EXPECT_EQ(valueDecl->initializer->astKind, ASTKind::AMBIGUOUS_FORCED_CAST_EXPR);
    ASSERT_NE(valueDecl->initializer->desugarExpr, nullptr);
    auto* binaryExpr = DynamicCast<BinaryExpr*>(valueDecl->initializer->desugarExpr.get());
    ASSERT_NE(binaryExpr, nullptr);
    EXPECT_EQ(binaryExpr->op, TokenKind::MUL);
    EXPECT_EQ(binaryExpr->GetTy()->String(), "Int64");
}

TEST_F(TypeCheckerTest, ForcedCastAmbiguousExprSelectsForcedCast)
{
    std::string code = R"(
public interface Runtime<T> where T <: Runtime<T> {
    static func memberAccess(e: Extern<T>, field: String): Extern<T>
    static func indexAccess(e: Extern<T>, arg: Any): Extern<T>
    static func memberUpdate(e: Extern<T>, field: String, value: Any): Unit
    static func indexUpdate(e: Extern<T>, field: Any, value: Any): Unit
    static func functionCall(e: Extern<T>, args: Array<Any>): Extern<T>
    static func fromExtern<R>(h: Extern<T>): R
    static func toExtern<R>(v: R): Extern<T>
}

public struct Extern<T> where T <: Runtime<T> {
    Extern(public let content: Any) {}
}

public class foo {}
public class DummyRuntime <: Runtime<DummyRuntime> {
    public static func memberAccess(e: Extern<DummyRuntime>, field: String): Extern<DummyRuntime> {
        return e
    }

    public static func indexAccess(e: Extern<DummyRuntime>, arg: Any): Extern<DummyRuntime> {
        return e
    }

    public static func memberUpdate(e: Extern<DummyRuntime>, field: String, value: Any): Unit {
    }

    public static func indexUpdate(e: Extern<DummyRuntime>, field: Any, value: Any): Unit {
    }

    public static func functionCall(e: Extern<DummyRuntime>, args: Array<Any>): Extern<DummyRuntime> {
        return e
    }

    public static func fromExtern<R>(h: Extern<DummyRuntime>): R {
        throw Exception()
    }

    public static func toExtern<R>(v: R): Extern<DummyRuntime> {
        return Extern<DummyRuntime>(v)
    }
}

func makeExtern(): Extern<DummyRuntime> {
    return Extern<DummyRuntime>(0)
}

main() {
    let externValue = makeExtern()
    let value = (foo)(externValue)
}
)";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    ASSERT_TRUE(instance->Compile(CompileStage::SEMA));
    ASSERT_EQ(diag.GetErrorCount(), 0);

    auto* valueDecl = FindVarDeclByName(*instance->GetSourcePackages()[0]->files[0], "value");
    ASSERT_NE(valueDecl, nullptr);
    ASSERT_NE(valueDecl->initializer, nullptr);
    EXPECT_EQ(valueDecl->initializer->astKind, ASTKind::AMBIGUOUS_FORCED_CAST_EXPR);
    auto* callExpr = RequireDesugaredCall(*valueDecl->initializer);
    ASSERT_NE(callExpr, nullptr);
    ASSERT_NE(callExpr->resolvedFunction, nullptr);
    EXPECT_EQ(callExpr->resolvedFunction->identifier.Val(), "fromExtern");
    auto* memberAccess = DynamicCast<MemberAccess*>(callExpr->baseFunc.get());
    ASSERT_NE(memberAccess, nullptr);
    EXPECT_EQ(memberAccess->field.Val(), "fromExtern");
    ASSERT_EQ(callExpr->args.size(), 1);
    auto* argParenExpr = DynamicCast<ParenExpr*>(callExpr->args[0]->expr.get());
    ASSERT_NE(argParenExpr, nullptr);
    auto* argRefExpr = DynamicCast<RefExpr*>(argParenExpr->expr.get());
    ASSERT_NE(argRefExpr, nullptr);
    EXPECT_EQ(argRefExpr->ref.identifier.Val(), "externValue");
}

TEST_F(TypeCheckerTest, ForcedCastAmbiguousFallsBackAndKeepsOriginalFailureForTypeCall)
{
    std::string code = R"(
public class foo {}

main() {
    let value = (foo)(1)
}
)";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    EXPECT_FALSE(instance->Compile(CompileStage::SEMA));
    EXPECT_GT(diag.GetErrorCount(), 0);
}

TEST_F(TypeCheckerTest, MacroDeclTest)
{
    std::string code = R"(
    public macro
    )";

    instance->code = code;
    instance->PerformParse();
    instance->PerformImportPackage();
    instance->PerformSema();
    std::vector<uint8_t> astData;
    instance->importManager->ExportAST(false, astData, *instance->GetSourcePackages()[0]);

    EXPECT_EQ(diag.GetErrorCount(), 3);
}

TEST_F(TypeCheckerTest, MacroCallInLSPTest)
{
    std::string code = R"(
    @M1
    func test() {
        var a = 1
        a = 2
    }
    @M2
    class A {
        var a = 1
        func test() {
            this.a = 2
        }
        func test1() {
            var b = 1
            b = 2
        }
    }
    @M3
    enum E {
        EE
        func test() {
            let a = EE
        }
    }
    main() {0}
    )";

    instance->code = code;
    instance->invocation.globalOptions.enableMacroInLSP = true;
    instance->Compile(CompileStage::SEMA);
    // Test MemberAccess has target or not in macrocall.
    auto visitPre = [&](Ptr<Node> curNode) -> VisitAction {
        if (curNode->astKind == ASTKind::MEMBER_ACCESS) {
            auto ma = StaticAs<ASTKind::MEMBER_ACCESS>(curNode);
            EXPECT_TRUE(ma->target);
            if (ma->target) {
                std::cout << "MemberAccess field " << ma->field.Val() << " has target." << std::endl;
            } else {
                std::cout << "MemberAccess field " << ma->field.Val() << " has no target." << std::endl;
            }
        }
        if (curNode->astKind == ASTKind::REF_EXPR) {
            auto re = StaticAs<ASTKind::REF_EXPR>(curNode);
            if (re->ref.identifier == "M1" || re->ref.identifier == "M2" || re->ref.identifier == "M3") {
                return VisitAction::SKIP_CHILDREN;
            }
            EXPECT_TRUE(re->ref.target);
            if (re->ref.target) {
                std::cout << re->ref.identifier.Val() << " has target." << std::endl;
            } else {
                std::cout << re->ref.identifier.Val() << " has no target." << std::endl;
            }
            return VisitAction::SKIP_CHILDREN;
        }
        return VisitAction::WALK_CHILDREN;
    };
    Walker macroCallWalker(instance->GetSourcePackages()[0]->files[0].get(), visitPre);
    macroCallWalker.Walk();
    // error: undeclared identifier 'M1'/'M2'/'M3'.
    EXPECT_EQ(diag.GetErrorCount(), 3);
    Cangjie::MacroProcMsger::GetInstance().CloseMacroSrv();
}

TEST_F(TypeCheckerTest, MacroDiagInLSPTest)
{
    instance->invocation.globalOptions.enableMacroInLSP = true;
    srcPath = projectPath + "/unittests/Sema/SemaCangjieFiles/";
    invocation.globalOptions.executablePath = projectPath + "/output/bin/";
    instance->compileOnePackageFromSrcFiles = true;

    instance->srcFilePaths = {srcPath + "UnableToInferGenericArgumentInTest.cj"};
    invocation.globalOptions.outputMode = GlobalOptions::OutputMode::STATIC_LIB;
    invocation.globalOptions.enableCompileTest = true;
    instance->Compile(CompileStage::SEMA);
    auto dv = diag.GetCategoryDiagnostic(DiagCategory::SEMA);
    bool findOptNone = false;
    for (auto& d : dv) {
        for (auto& n : d.subDiags) {
            if (n.mainHint.str.find("Enum-Option<Int64>") != std::string::npos) {
                findOptNone = true;
                EXPECT_EQ(n.mainHint.range.end.line, 14);
                EXPECT_EQ(n.mainHint.range.end.column, 55);
            }
        }
    }
    EXPECT_EQ(findOptNone, true);
    Cangjie::MacroProcMsger::GetInstance().CloseMacroSrv();
}

TEST_F(TypeCheckerTest, DISABLED_NoDiagInLSPMacroCallTest)
{
    srcPath = projectPath + "/unittests/Sema/SemaCangjieFiles/";
    std::string command = "cjc " + srcPath + "AddClassTyInfoMacro.cj --compile-macro -Woff all";
    int err = system(command.c_str());
    ASSERT_EQ(0, err);

    instance->invocation.globalOptions.enableMacroInLSP = false;
    invocation.globalOptions.executablePath = projectPath + "/output/bin/";
    instance->compileOnePackageFromSrcFiles = true;

    instance->srcFilePaths = {srcPath + "NoDiagInLSPMacroCall.cj"};
    invocation.globalOptions.outputMode = GlobalOptions::OutputMode::STATIC_LIB;
    invocation.globalOptions.enableCompileTest = true;
    instance->Compile(CompileStage::SEMA);
    EXPECT_EQ(diag.GetErrorCount(), 0);
    Cangjie::MacroProcMsger::GetInstance().CloseMacroSrv();
}

TEST_F(TypeCheckerTest, NoDiagInLSPMacroCallForTest)
{
    srcPath = projectPath + "/unittests/Sema/SemaCangjieFiles/";
    std::string command = "cjc " + srcPath + "ModifyClassBuildFunc.cj --compile-macro -Woff all";
    int err = system(command.c_str());
    ASSERT_EQ(0, err);

    instance->invocation.globalOptions.enableMacroInLSP = true;
    invocation.globalOptions.executablePath = projectPath + "/output/bin/";
    instance->compileOnePackageFromSrcFiles = true;

    instance->srcFilePaths = {srcPath + "NoDiagInLSPMacroCallNode.cj"};
    invocation.globalOptions.outputMode = GlobalOptions::OutputMode::STATIC_LIB;
    invocation.globalOptions.enableCompileTest = true;
    instance->Compile(CompileStage::SEMA);
    EXPECT_EQ(diag.GetErrorCount(), 0);
    Cangjie::MacroProcMsger::GetInstance().CloseMacroSrv();
}

TEST_F(TypeCheckerTest, MacroCallOfTopLevelInLSPTest)
{
    std::string code = R"(
    @M1
    func test(v: String) {
        var a = 1
        a = 2
    }
    main() {0}
    )";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    instance->invocation.globalOptions.enableMacroInLSP = true;
    instance->Compile(CompileStage::IMPORT_PACKAGE);
    // Skip macro expand stage and move node in 'originalMacroCallNodes' to simulate original macro code for LSP.
    auto file = instance->GetSourcePackages()[0]->files[0].get();
    for (auto& it : file->decls) {
        if (auto md = DynamicCast<MacroExpandDecl>(it.get())) {
            file->originalMacroCallNodes.emplace_back(std::move(md->invocation.decl));
        }
    }
    instance->PerformSema();
    unsigned checkCount = 0;
    // Test ty is set correctly.
    auto visitPre = [&](Ptr<Node> curNode) -> VisitAction {
        if (auto fp = DynamicCast<FuncParam>(curNode); fp && fp->identifier == "v") {
            checkCount++;
            EXPECT_TRUE(fp->type != nullptr);
            if (fp->type != nullptr) {
                EXPECT_EQ(fp->type->GetTy()->String(), "Struct-String");
            }
        }
        return VisitAction::WALK_CHILDREN;
    };
    for (auto& it : file->originalMacroCallNodes) {
        Walker(it.get(), visitPre).Walk();
    }
    EXPECT_EQ(checkCount, 1);
    Cangjie::MacroProcMsger::GetInstance().CloseMacroSrv();
}

TEST_F(TypeCheckerTest, AssumptionTest)
{
#ifdef _WIN32
    srcPath = projectPath + "\\unittests\\Sema\\SemaCangjieFiles\\AssumptionTest";
#elif defined(__unix__)
    srcPath = projectPath + "/unittests/Sema/SemaCangjieFiles/AssumptionTest";
#endif

    instance->srcDirs = {srcPath};
    instance->invocation.globalOptions.implicitPrelude = true;
    instance->Compile();

    for (auto& decl : instance->GetSourcePackages()[0]->files[0]->decls) {
        if (auto cd = AST::As<ASTKind::CLASS_DECL>(decl.get()); cd) {
            if (cd->identifier == "A") {
                EXPECT_EQ(cd->generic->assumptionCollection.size(), 1);
                auto it = cd->generic->assumptionCollection.begin();
                EXPECT_EQ(it->second.size(), 1);
            } else if (cd->identifier == "B") {
                EXPECT_EQ(cd->generic->assumptionCollection.size(), 1);
                auto it = cd->generic->assumptionCollection.begin();
                EXPECT_EQ(it->second.size(), 2);
            } else if (cd->identifier == "D") {
                for (auto& it : cd->generic->assumptionCollection) {
                    if (dynamic_cast<GenericsTy*>(it.first.get())->name == "V") {
                        EXPECT_EQ(it.second.size(), 2);
                    }
                    if (dynamic_cast<GenericsTy*>(it.first.get())->name == "U") {
                        EXPECT_EQ(it.second.size(), 3);
                    }
                }
            } else if (cd->identifier == "E") {
                EXPECT_EQ(cd->generic->assumptionCollection.size(), 1);
                auto it = cd->generic->assumptionCollection.begin();
                EXPECT_EQ(it->second.size(), 1);
            } else if (cd->identifier == "F") {
                EXPECT_EQ(cd->generic->assumptionCollection.size(), 1);
                auto it = cd->generic->assumptionCollection.begin();
                EXPECT_EQ(it->second.size(), 2);
            } else if (cd->identifier == "G") {
                EXPECT_EQ(cd->generic->assumptionCollection.size(), 1);
                auto it = cd->generic->assumptionCollection.begin();
                EXPECT_EQ(it->second.size(), 2);
            }
        }
    }
}

TEST_F(TypeCheckerTest, DISABLED_SpawnTest)
{
#ifdef _WIN32
    srcPath = projectPath + "\\unittests\\Sema\\SemaCangjieFiles\\";
#elif defined(__unix__)
    srcPath = projectPath + "/unittests/Sema/SemaCangjieFiles/";
#endif
    auto srcFile = srcPath + "spawn.cj";
    std::string failedReason;
    auto content = FileUtil::ReadFileContent(srcFile, failedReason);
    if (!content.has_value()) {
        diag.DiagnoseRefactor(
            DiagKindRefactor::module_read_file_to_buffer_failed, DEFAULT_POSITION, srcFile, failedReason);
    }
    instance->code = std::move(content.value());
    instance->invocation.globalOptions.implicitPrelude = true;
    instance->Compile();

    auto expectedErrorCount = 0;
    EXPECT_EQ(diag.GetErrorCount(), expectedErrorCount);
    EXPECT_EQ(instance->GetSourcePackages()[0]->files.size(), 1);

    Ptr<Node> targetFutureObj1{nullptr};
    Ptr<Node> targetFutureObj2{nullptr};
    for (auto& decl : instance->GetSourcePackages()[0]->files[0]->decls) {
        auto main = As<ASTKind::FUNC_DECL>(decl.get());
        EXPECT_TRUE(main);
        EXPECT_TRUE(main->funcBody->body->body.size() == 4);
        targetFutureObj1 = main->funcBody->body->body[1].get();
        targetFutureObj2 = main->funcBody->body->body[2].get();
    }

    auto var1 = As<ASTKind::VAR_DECL>(targetFutureObj1);
    auto var2 = As<ASTKind::VAR_DECL>(targetFutureObj2);
    EXPECT_TRUE(var1);
    EXPECT_TRUE(var2);
    auto ty1 = dynamic_cast<ClassTy*>(var1->GetTy().get());
    auto ty2 = dynamic_cast<ClassTy*>(var2->GetTy().get());
    EXPECT_TRUE(ty1 && ty2 && ty1 == ty2);
    EXPECT_TRUE(ty1->decl->identifier == "Future");
    EXPECT_TRUE(ty1->typeArgs.size() == 1);
    EXPECT_TRUE(ty1->typeArgs[0]->kind == TypeKind::TYPE_INT64);
}

TEST_F(TypeCheckerTest, DiagnosticNodeTest)
{
    std::string code = R"(
interface I {
    func foo(): Unit
}

class C1 <: I {}
struct S1 <: I {}

main() {}
    )";

    instance->code = code;
    instance->invocation.globalOptions.implicitPrelude = true;
    instance->Compile(CompileStage::SEMA);

    auto diags = diag.GetCategoryDiagnostic(DiagCategory::SEMA);
    int foundDiag = 0;
    for (auto& d : diags) {
        if (d.rKind == DiagKindRefactor::sema_class_need_abstract_modifier_or_func_need_impl) {
            foundDiag++;
            EXPECT_TRUE(d.node != nullptr);
            EXPECT_EQ(d.node->astKind, ASTKind::CLASS_DECL);
            auto classDecl = StaticAs<ASTKind::CLASS_DECL>(d.node);
            EXPECT_EQ(classDecl->identifier.Val(), "C1");
            EXPECT_FALSE(d.subDiags.empty());
            auto noteNode = d.subDiags[0].GetNodeSubDiagAt();
            EXPECT_TRUE(noteNode != nullptr);
            EXPECT_EQ(noteNode->astKind, ASTKind::FUNC_DECL);
            auto funcDecl = StaticAs<ASTKind::FUNC_DECL>(noteNode);
            EXPECT_EQ(funcDecl->identifier.Val(), "foo");
            EXPECT_EQ(funcDecl->outerDecl->identifier.Val(), "I");
        }
        if (d.rKind == DiagKindRefactor::sema_need_member_implementation) {
            foundDiag++;
            EXPECT_TRUE(d.node != nullptr);
            EXPECT_EQ(d.node->astKind, ASTKind::STRUCT_DECL);
            auto structDecl = StaticAs<ASTKind::STRUCT_DECL>(d.node);
            EXPECT_EQ(structDecl->identifier.Val(), "S1");
            EXPECT_FALSE(d.subDiags.empty());
            auto noteNode = d.subDiags[0].GetNodeSubDiagAt();
            EXPECT_TRUE(noteNode != nullptr);
            EXPECT_EQ(noteNode->astKind, ASTKind::FUNC_DECL);
            auto funcDecl = StaticAs<ASTKind::FUNC_DECL>(noteNode);
            EXPECT_EQ(funcDecl->identifier.Val(), "foo");
            EXPECT_EQ(funcDecl->outerDecl->identifier.Val(), "I");
        }
    }
    EXPECT_EQ(foundDiag, 2);
}
