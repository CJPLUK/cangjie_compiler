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
const std::string EXTERN_INDEXED_ACCESS_CTOR = "ExternIndexedAccess";
const std::string EXTERN_MEMBER_UPDATE_CTOR = "ExternMemberUpdate";
const std::string EXTERN_INDEXED_UPDATE_CTOR = "ExternIndexedUpdate";
const std::string EXTERN_FUNCTION_CALL_CTOR = "ExternFunctionCall";
const std::string EXTERN_COMPOUND_ASSIGNMENT_CTOR = "ExternCompoundAssignment";

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
            if (auto ae = DynamicCast<AssignExpr*>(expr)) {
                PrepareStaticCompoundAssignment(*ae);
            }
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
        if (auto se = DynamicCast<const SubscriptExpr*>(&expr)) {
            return IsDynamicExternSubscript(*se);
        }
        if (auto ce = DynamicCast<const CallExpr*>(&expr)) {
            return IsDynamicExternCall(*ce);
        }
        if (auto ae = DynamicCast<const AssignExpr*>(&expr)) {
            return IsDynamicExternUpdate(*ae);
        }
        return false;
    }

    /** A primitive constructor of Extern<T>, with its type instantiated for a given T. */
    struct Ctor {
        Ptr<FuncDecl> decl{nullptr};
        Ptr<FuncTy> ty{nullptr};
    };

    /** Whether evaluating @p expr more than once has no observable effect. */
    static bool IsSideEffectFree(const Expr& expr)
    {
        auto target = expr.GetTarget();
        bool isPureTarget = target &&
            (target->IsTypeDecl() || target->astKind == ASTKind::PACKAGE_DECL ||
                (Is<VarDecl>(target) && !Is<PropDecl>(target)));
        if (expr.astKind == ASTKind::REF_EXPR) {
            return isPureTarget;
        }
        auto ma = DynamicCast<const MemberAccess*>(&expr);
        return ma && isPureTarget && ma->baseExpr && IsSideEffectFree(*ma->baseExpr);
    }

    void PrepareStaticCompoundAssignment(AssignExpr& ae);
    void Desugar(Expr& expr);
    OwnedPtr<Expr> BuildTree(Expr& expr);
    OwnedPtr<Expr> BuildOperation(Expr& expr, Ty& externTy);
    OwnedPtr<Expr> BuildCompoundAssignment(AssignExpr& ae, Ty& externTy);
    OwnedPtr<Expr> BuildArgs(CallExpr& ce, Ty& arrayTy);
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

/**
 * A compound assignment lhs op= v whose statically resolved left value lhs has type Extern<T> is type checked as
 * lhs = lhs'.op(v), where the copy lhs' is mapped to lhs so that the receiver of lhs is evaluated once. lhs' becomes
 * a leaf of the tree of the dynamic call and is read as a value, while the mapping yields a reference to lhs. So lhs'
 * is either evaluated again, if that has no effect, or the receiver of lhs is stored in a variable:
 * {
 *     let tmp = base
 *     tmp.x = tmp.x.op(v)
 * }
 */
void ExternOperations::PrepareStaticCompoundAssignment(AssignExpr& ae)
{
    auto inner = ae.isCompound ? DynamicCast<AssignExpr*>(ae.desugarExpr.get()) : nullptr;
    auto ce = inner ? DynamicCast<CallExpr*>(inner->rightExpr.get()) : nullptr;
    auto callee = ce && IsDynamic(*ce) ? DynamicCast<MemberAccess*>(ce->baseFunc.get()) : nullptr;
    if (!callee || !callee->baseExpr || callee->baseExpr->mapExpr != inner->leftValue.get()) {
        return;
    }
    auto& copy = *callee->baseExpr;
    copy.mapExpr = nullptr;
    auto lhs = DynamicCast<MemberAccess*>(inner->leftValue.get());
    if (!lhs || !lhs->baseExpr || IsSideEffectFree(*lhs->baseExpr)) {
        return;
    }
    auto vd = CreateVarDecl(V_COMPILER, std::move(lhs->baseExpr));
    vd->fullPackageName = ae.GetFullPackageName();
    CopyBasicInfo(vd->initializer.get(), vd.get());
    lhs->baseExpr = CreateRefExpr(*vd, *vd->initializer);
    CopyBasicInfo(vd->initializer.get(), lhs->baseExpr.get());
    auto& copyAccess = StaticCast<MemberAccess&>(copy);
    copyAccess.baseExpr = CreateRefExpr(*vd, *vd->initializer);
    CopyBasicInfo(vd->initializer.get(), copyAccess.baseExpr.get());
    std::vector<OwnedPtr<Node>> nodes;
    nodes.emplace_back(std::move(vd));
    nodes.emplace_back(std::move(ae.desugarExpr));
    auto block = CreateBlock(std::move(nodes), ae.GetTy());
    CopyBasicInfo(&ae, block.get());
    AddCurFile(*block, ae.curFile);
    ae.desugarExpr = std::move(block);
}

OwnedPtr<Expr> ExternOperations::BuildTree(Expr& expr)
{
    if (auto pe = DynamicCast<ParenExpr*>(&expr); pe && pe->expr && IsDynamic(*pe->expr)) {
        return BuildTree(*pe->expr);
    }
    if (!IsDynamic(expr)) {
        return ASTCloner::Clone(Ptr(&expr));
    }
    return BuildOperation(expr, *expr.GetTy());
}

OwnedPtr<Expr> ExternOperations::BuildOperation(Expr& expr, Ty& externTy)
{
    // An update is built like the access to its left value, with the assigned value as an extra argument.
    auto ae = DynamicCast<AssignExpr*>(&expr);
    if (ae && ae->isCompound) {
        return BuildCompoundAssignment(*ae, externTy);
    }
    auto& access = ae ? *ae->leftValue : expr;
    Ctor ctor;
    std::vector<OwnedPtr<Expr>> args;
    if (auto ma = DynamicCast<MemberAccess*>(&access)) {
        ctor = LookupCtor(ae ? EXTERN_MEMBER_UPDATE_CTOR : EXTERN_MEMBER_ACCESS_CTOR, externTy);
        if (!ctor.decl) {
            return nullptr;
        }
        auto field = CreateLitConstExpr(LitConstKind::STRING, ma->field.Val(), ctor.ty->paramTys[1], true);
        CopyBasicInfo(ma, field.get());
        args.emplace_back(BuildTree(*ma->baseExpr));
        args.emplace_back(std::move(field));
    } else if (auto se = DynamicCast<SubscriptExpr*>(&access)) {
        // e[i1, ..., in] is built like e[i1]...[in].
        auto receiver = BuildTree(*se->baseExpr);
        auto accessCtor = LookupCtor(EXTERN_INDEXED_ACCESS_CTOR, externTy);
        for (size_t i = 0; i + 1 < se->indexExprs.size(); ++i) {
            auto index = BuildTree(*se->indexExprs[i]);
            if (!accessCtor.decl || !receiver || !index) {
                return nullptr;
            }
            std::vector<OwnedPtr<Expr>> accessArgs;
            accessArgs.emplace_back(std::move(receiver));
            accessArgs.emplace_back(std::move(index));
            receiver = CreateCtorCall(accessCtor, externTy, std::move(accessArgs), *se);
        }
        ctor = ae ? LookupCtor(EXTERN_INDEXED_UPDATE_CTOR, externTy) : accessCtor;
        args.emplace_back(std::move(receiver));
        args.emplace_back(BuildTree(*se->indexExprs.back()));
    } else {
        auto& ce = StaticCast<CallExpr&>(expr);
        ctor = LookupCtor(EXTERN_FUNCTION_CALL_CTOR, externTy);
        if (!ctor.decl) {
            return nullptr;
        }
        args.emplace_back(BuildTree(*ce.baseFunc));
        args.emplace_back(BuildArgs(ce, *ctor.ty->paramTys[1]));
    }
    if (ae) {
        args.emplace_back(BuildTree(*ae->rightExpr));
    }
    if (!ctor.decl || std::any_of(args.begin(), args.end(), [](auto& arg) { return !arg; })) {
        return nullptr;
    }
    return CreateCtorCall(ctor, externTy, std::move(args), expr);
}

OwnedPtr<Expr> ExternOperations::BuildCompoundAssignment(AssignExpr& ae, Ty& externTy)
{
    auto ctor = LookupCtor(EXTERN_COMPOUND_ASSIGNMENT_CTOR, externTy);
    if (!ctor.decl) {
        return nullptr;
    }
    // e.f op= v is built from the access e.f and the binary operator op, without '='.
    auto target = BuildOperation(*ae.leftValue, externTy);
    auto op = CreateLitConstExpr(LitConstKind::STRING,
        TOKENS[static_cast<int>(COMPOUND_ASSIGN_EXPR_MAP.at(ae.op))], ctor.ty->paramTys[1], true);
    CopyBasicInfo(&ae, op.get());
    auto value = BuildTree(*ae.rightExpr);
    if (!target || !value) {
        return nullptr;
    }
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(std::move(target));
    args.emplace_back(std::move(op));
    args.emplace_back(std::move(value));
    return CreateCtorCall(ctor, externTy, std::move(args), ae);
}

OwnedPtr<Expr> ExternOperations::BuildArgs(CallExpr& ce, Ty& arrayTy)
{
    std::vector<OwnedPtr<Expr>> elements;
    for (auto& arg : ce.args) {
        auto element = BuildTree(*arg->expr);
        if (!element) {
            return nullptr;
        }
        elements.emplace_back(std::move(element));
    }
    auto arrayLit = CreateArrayLit(std::move(elements), &arrayTy);
    AddArrayLitConstructor(*arrayLit);
    if (!arrayLit->initFunc) {
        return nullptr;
    }
    CopyBasicInfo(&ce, arrayLit.get());
    arrayLit->EnableAttr(Attribute::IMPLICIT_ADD);
    return arrayLit;
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
        auto argTy = args[i]->GetTy();
        auto arg = CreateFuncArg(std::move(args[i]), "", argTy);
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
