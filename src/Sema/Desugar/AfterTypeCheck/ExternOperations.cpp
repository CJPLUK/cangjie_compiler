// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the desugaring of dynamic operations on Extern<T> values: the outermost dynamic operation is
 * rewritten into T.eval(tree), where tree is built from the primitive constructors of Extern<T>. Nested dynamic
 * operations become nodes of the tree, any other expression is kept as a leaf and desugared on its own.
 */

#include "TypeCheckerImpl.h"

#include "Desugar/AfterTypeCheck.h"
#include "TypeCheckUtil.h"

#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Utils.h"
#include "cangjie/AST/Walker.h"

using namespace Cangjie;
using namespace AST;
using namespace TypeCheckUtil;

namespace {
const std::string EVAL_FUNC = "eval";
const std::string EXTERN_MEMBER_ACCESS_CTOR = "ExternMemberAccess";

class ExternOperations {
public:
    using EvalLookup = std::function<ForeignRuntimeFunc(Ty& runtimeTy, Ptr<const File> file)>;

    ExternOperations(TypeManager& typeManager, EvalLookup lookup) : typeManager(typeManager), lookup(std::move(lookup))
    {
    }

    void Run(Node& root)
    {
        std::function<VisitAction(Ptr<Node>)> preVisit = [this](Ptr<Node> node) -> VisitAction {
            auto expr = DynamicCast<Expr*>(node);
            if (!expr || !IsDynamic(*expr)) {
                return VisitAction::WALK_CHILDREN;
            }
            Desugar(*expr);
            return VisitAction::SKIP_CHILDREN;
        };
        Walker(&root, preVisit).Walk();
    }

private:
    static bool IsDynamic(const Expr& expr)
    {
        if (expr.desugarExpr) {
            return false;
        }
        if (auto ma = DynamicCast<const MemberAccess*>(&expr)) {
            return IsDynamicExternMemberAccess(*ma);
        }
        return false;
    }

    /** A primitive constructor of Extern<T>, with its type instantiated for a given T. */
    struct Ctor {
        Ptr<FuncDecl> decl{nullptr};
        Ptr<FuncTy> ty{nullptr};
    };

    void Desugar(Expr& expr);
    OwnedPtr<Expr> BuildTree(Expr& expr);
    Ctor LookupCtor(const std::string& name, Ty& externTy);
    static OwnedPtr<CallExpr> CreateCtorCall(
        const Ctor& ctor, Ty& externTy, std::vector<OwnedPtr<Expr>> args, const Node& pos);
    OwnedPtr<CallExpr> CreateEvalCall(OwnedPtr<Expr> tree, Ty& externTy, const Expr& pos);

    TypeManager& typeManager;
    EvalLookup lookup;
};

void ExternOperations::Desugar(Expr& expr)
{
    auto tree = BuildTree(expr);
    auto eval = tree ? CreateEvalCall(std::move(tree), *expr.GetTy(), expr) : nullptr;
    CJC_NULLPTR_CHECK(eval);
    expr.desugarExpr = std::move(eval);
    AddCurFile(*expr.desugarExpr, expr.curFile);
    // Leaves of the tree may contain dynamic operations of their own.
    Run(*expr.desugarExpr);
}

OwnedPtr<Expr> ExternOperations::BuildTree(Expr& expr)
{
    if (auto pe = DynamicCast<ParenExpr*>(&expr); pe && pe->expr && IsDynamic(*pe->expr)) {
        return BuildTree(*pe->expr);
    }
    if (!IsDynamic(expr)) {
        return ASTCloner::Clone(Ptr(&expr));
    }
    auto& ma = StaticCast<MemberAccess&>(expr);
    auto ctor = LookupCtor(EXTERN_MEMBER_ACCESS_CTOR, *ma.GetTy());
    auto receiver = BuildTree(*ma.baseExpr);
    if (!ctor.decl || !receiver) {
        return nullptr;
    }
    auto field = CreateLitConstExpr(LitConstKind::STRING, ma.field.Val(), ctor.ty->paramTys[1], true);
    CopyBasicInfo(&ma, field.get());
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(std::move(receiver));
    args.emplace_back(std::move(field));
    return CreateCtorCall(ctor, *ma.GetTy(), std::move(args), ma);
}

ExternOperations::Ctor ExternOperations::LookupCtor(const std::string& name, Ty& externTy)
{
    auto externDecl = Ty::GetDeclOfTy(&externTy);
    auto ctor = DynamicCast<FuncDecl*>(Sema::Desugar::AfterTypeCheck::LookupEnumMember(externDecl, name));
    if (!ctor || !Ty::IsTyCorrect(ctor->GetTy())) {
        return {};
    }
    auto ctorTy = DynamicCast<FuncTy*>(
        typeManager.GetInstantiatedTy(ctor->GetTy(), GenerateTypeMapping(*externDecl, externTy.typeArgs)));
    return ctorTy ? Ctor{ctor, ctorTy} : Ctor{};
}

OwnedPtr<CallExpr> ExternOperations::CreateCtorCall(
    const Ctor& ctor, Ty& externTy, std::vector<OwnedPtr<Expr>> args, const Node& pos)
{
    CJC_ASSERT(ctor.decl && ctor.ty && ctor.ty->paramTys.size() == args.size());
    auto ctorRef = CreateRefExpr(*ctor.decl);
    ctorRef->SetTy(ctor.ty);
    ctorRef->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&pos, ctorRef.get());
    std::vector<OwnedPtr<FuncArg>> funcArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        auto arg = CreateFuncArg(std::move(args[i]), "", ctor.ty->paramTys[i]);
        CopyBasicInfo(&pos, arg.get());
        funcArgs.emplace_back(std::move(arg));
    }
    auto ce = CreateCallExpr(
        std::move(ctorRef), std::move(funcArgs), ctor.decl, &externTy, CallKind::CALL_DECLARED_FUNCTION);
    ce->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&pos, ce.get());
    return ce;
}

OwnedPtr<CallExpr> ExternOperations::CreateEvalCall(OwnedPtr<Expr> tree, Ty& externTy, const Expr& pos)
{
    CJC_ASSERT(externTy.IsCoreExternType());
    auto runtimeTy = externTy.typeArgs[0];
    auto [eval, matchedParentTy] = lookup(*runtimeTy, pos.curFile);
    if (!eval) {
        return nullptr;
    }
    auto baseFunc = Sema::Desugar::AfterTypeCheck::CreateForeignRuntimeFuncAccess(
        *runtimeTy, *eval, matchedParentTy, *typeManager.GetFunctionTy({&externTy}, &externTy), pos);
    if (!baseFunc) {
        return nullptr;
    }
    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateFuncArg(std::move(tree), "", &externTy));
    auto ce = CreateCallExpr(std::move(baseFunc), std::move(args), eval, &externTy, CallKind::CALL_DECLARED_FUNCTION);
    ce->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&pos, ce.get());
    return ce;
}
} // namespace

void TypeChecker::TypeCheckerImpl::DesugarExternOperations(ASTContext& ctx, Package& pkg)
{
    auto lookup = [this, &ctx](Ty& runtimeTy, Ptr<const File> file) {
        return LookupForeignRuntimeFunc(ctx, runtimeTy, EVAL_FUNC, file);
    };
    ExternOperations(typeManager, lookup).Run(pkg);
}
