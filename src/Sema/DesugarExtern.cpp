// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the functions to check decl attributes.
 */

#include "TypeCheckerImpl.h"

#include "Diags.h"
#include "TypeCheckUtil.h"
#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "DiagSuppressor.h"
#include "Diags.h"

namespace Cangjie {
using namespace TypeCheckUtil;
using namespace AST;
using namespace Sema;

OwnedPtr<Expr> CloneEffectiveExpr(Ptr<Expr> expr)
{
    OwnedPtr<Expr> cloned;
    if (expr && expr->desugarExpr) {
        cloned = ASTCloner::Clone(Ptr(expr->desugarExpr.get()));
    } else {
        cloned = ASTCloner::Clone(expr);
    }
    if (cloned) {
        cloned->EnableAttr(Attribute::EXTERN_DESUGAR);
    }
    return cloned;
}

// `x` -> `T.toExtern(x)` in a context of type `Extern<T>`
bool TypeChecker::TypeCheckerImpl::CoerceToExtern(ASTContext& ctx, Ptr<Ty> targetTy, Ptr<Expr> nodeExpr)
{
    CJC_ASSERT(TypeIsExtern(targetTy));
    CJC_ASSERT(targetTy->typeArgs.size() == 1);

    Synthesize({ctx, SynPos::EXPR_ARG}, nodeExpr);
    ReplaceIdealTy(*nodeExpr);

    auto sourceTy = nodeExpr->GetTy();
    // Typechecking of the inner node failed, so we fail
    if (!Ty::IsTyCorrect(sourceTy)) {
        nodeExpr->SetTy(TypeManager::GetInvalidTy());
        return false;
    }

    // Node is extern as well, nothing to be done
    if (typeManager.IsSubtype(sourceTy, targetTy)) {
        return true;
    }

    // Runtime type T in Extern<T>.
    auto info = ResolveExternRuntime(targetTy);
    auto runtimeTy = info.runtimeTy;

    OwnedPtr<Expr> inner;
    if (nodeExpr->desugarExpr) {
        inner = std::move(nodeExpr->desugarExpr);
    } else {
        inner = ASTCloner::Clone(Ptr(nodeExpr));
        inner->SetTy(sourceTy);
        inner->EnableAttr(Attribute::IS_CHECK_VISITED, Attribute::EXTERN_DESUGAR);
    }

    // The array of arguments, which grabs wahatevet the inner expression of node is -- due to the fact that
    // node could be desugared already
    std::vector<OwnedPtr<FuncArg>> args = {};
    args.emplace_back(CreateExternDesugarArg(std::move(inner), sourceTy));

    // Grab the reference to the runtime type from the Extern
    // Ie, if target is Extern<T>, this grabs T
    auto runtimeRef = CreateExternRuntimeRef(*nodeExpr, info);

    // This yields T.toExtern. Generic type parameters are resolved by normal member lookup.
    OwnedPtr<MemberAccess> toExtern;
    Ptr<FuncDecl> toExternDecl = nullptr;
    if (info.isGeneric) {
        toExtern = MakeOwned<MemberAccess>();
        toExtern->baseExpr = std::move(runtimeRef);
        toExtern->field = "toExtern";
        toExternDecl = GetRuntimeFuncDecl("toExtern");
        auto runtimeInterfaceDecl = importManager.GetCoreDecl<InterfaceDecl>("Runtime");
        CJC_ASSERT(runtimeInterfaceDecl);
        auto typeMapping = GenerateTypeMapping(*runtimeInterfaceDecl, {runtimeTy});
        toExtern->SetTy(typeManager.GetInstantiatedTy(toExternDecl->GetTy(), typeMapping));
    } else {
        toExtern = CreateMemberAccess(std::move(runtimeRef), "toExtern");
        toExternDecl = DynamicCast<FuncDecl*>(toExtern->target);
    }
    toExtern->isAlone = false;
    toExtern->EnableAttr(Attribute::COMPILER_ADD, Attribute::EXTERN_DESUGAR);
    CopyBasicInfo(nodeExpr, toExtern.get());
    CJC_ASSERT(toExternDecl);

    // This is the the type parameter, as in T.toExtern<sourceTy>
    toExtern->instTys.emplace_back(sourceTy);
    auto instantiatedToExternTy = typeManager.GetFunctionTy({sourceTy}, targetTy);
    toExtern->target = toExternDecl;
    toExtern->SetTy(instantiatedToExternTy);
    // This adds the toExternDecl to the sets of overload targets, we need this
    // as we are gonna pass this into Synthesise that runs inference for the entire tree.
    if (!info.isGeneric) {
        toExtern->targets.emplace_back(toExternDecl);
    }

    // The actual call expressions, at last. Generic runtime calls must not be pre-resolved here;
    // overload resolution needs to infer the Runtime<T> method from T.toExtern.
    auto callTarget = info.isGeneric ? nullptr : toExternDecl;
    auto call =
        CreateCallExpr(std::move(toExtern), std::move(args), callTarget, targetTy, CallKind::CALL_DECLARED_FUNCTION);
    CopyBasicInfo(nodeExpr, call.get());
    call->sourceExpr = nodeExpr;
    call->resolvedFunction = toExternDecl;
    call->EnableAttr(Attribute::EXTERN_DESUGAR);

    // Make sure everything ends up well
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "toExtern" &&
            typeManager.IsSubtype(call->GetTy(), targetTy)));

    nodeExpr->SetTy(targetTy);
    call->SetTy(targetTy);
    nodeExpr->desugarExpr = std::move(call);
    return true;
}

// `e.foo` -> `T.memberAccess("foo")` for `e: Extern<T>`
bool TypeChecker::TypeCheckerImpl::TryDesugarExternMemberAccess(ASTContext& ctx, MemberAccess& ma)
{
    (void)ctx;
    CJC_NULLPTR_CHECK(ma.baseExpr);
    auto sourceExternTy = ma.baseExpr->GetTy();
    if (!TypeIsExtern(sourceExternTy)) {
        return false;
    }
    if (ma.TestAttr(Attribute::LEFT_VALUE)) {
        if (auto baseRef = DynamicCast<RefExpr*>(ma.baseExpr.get()); baseRef && baseRef->isThis) {
            // this corresponds to the `this.payload` expression, part of `this.payload = payload`,
            // in the core library which is a normal field, and should be left alone.
            CJC_ASSERT(ma.field == "payload");
            return false;
        } else {
            ma.SetTy(sourceExternTy);
            return true;
        }
    }
    if (ma.callOrPattern) {
        return false;
    }

    auto info = ResolveExternRuntime(sourceExternTy);

    Ptr<FuncDecl> memberAccessDecl = nullptr;
    auto memberAccess = CreateRuntimeMemberAccess(ma, info, "memberAccess", memberAccessDecl);

    OwnedPtr<Expr> namedExpr = ma.baseExpr->desugarExpr ? ASTCloner::Clone(Ptr(ma.baseExpr->desugarExpr.get()))
                                                        : ASTCloner::Clone(Ptr(ma.baseExpr.get()));

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(namedExpr)));
    args.emplace_back(CreateExternDesugarArg(CreateStringLit(ma.field.Val())));

    auto call = CreateRuntimeCall(ma, info, std::move(memberAccess), memberAccessDecl, std::move(args), sourceExternTy);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "memberAccess" &&
            typeManager.IsSubtype(call->GetTy(), sourceExternTy)));

    ma.SetTy(sourceExternTy);
    call->SetTy(sourceExternTy);
    ma.desugarExpr = std::move(call);
    return true;
}

// `e[idx]` -> `T.indexAccess(idx)` for `e: Extern<T>`
bool TypeChecker::TypeCheckerImpl::TryDesugarExternIndexAccess(ASTContext& ctx, Ptr<Ty> target, SubscriptExpr& se)
{
    (void)target;
    CJC_NULLPTR_CHECK(se.baseExpr);
    auto typecheck = [this, &ctx](Ptr<Node> node) {
        auto ty = node->GetTy();
        if (!Ty::IsTyCorrect(ty)) {
            ty = Synthesize({ctx, SynPos::EXPR_ARG}, node);
        }
        ReplaceIdealTy(*node);
        return node->GetTy();
    };
    auto sourceExternTy = typecheck(se.baseExpr.get());
    for (auto& indexExpr : se.indexExprs) {
        CJC_NULLPTR_CHECK(indexExpr);
        auto indexTy = typecheck(indexExpr.get());
        if (!Ty::IsTyCorrect(indexTy)) {
            se.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
    }
    if (!TypeIsExtern(sourceExternTy)) {
        return false;
    }
    if (se.TestAttr(Attribute::LEFT_VALUE)) {
        se.SetTy(sourceExternTy);
        return true;
    }

    auto info = ResolveExternRuntime(sourceExternTy);

    OwnedPtr<Expr> indexedExpr = se.baseExpr->desugarExpr ? ASTCloner::Clone(Ptr(se.baseExpr->desugarExpr.get()))
                                                          : ASTCloner::Clone(Ptr(se.baseExpr.get()));
    for (auto& indexExpr : se.indexExprs) {
        Ptr<FuncDecl> indexAccessDecl = nullptr;
        auto indexAccess = CreateRuntimeMemberAccess(se, info, "indexAccess", indexAccessDecl);

        std::vector<OwnedPtr<FuncArg>> args;
        args.emplace_back(CreateExternDesugarArg(std::move(indexedExpr)));
        args.emplace_back(CreateExternDesugarArg(ASTCloner::Clone(Ptr(indexExpr.get()))));

        auto call =
            CreateRuntimeCall(se, info, std::move(indexAccess), indexAccessDecl, std::move(args), sourceExternTy);
        call->SetTy(sourceExternTy);
        indexedExpr = std::move(call);
    }

    auto call = DynamicCast<CallExpr*>(indexedExpr.get());
    CJC_ASSERT(call);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "indexAccess" &&
            typeManager.IsSubtype(call->GetTy(), sourceExternTy)));

    se.SetTy(sourceExternTy);
    indexedExpr->SetTy(sourceExternTy);
    se.desugarExpr = std::move(indexedExpr);
    return true;
}
// `e.foo = v` -> `T.memberUpdate("foo", v)` for `e: Extern<T>`
bool TypeChecker::TypeCheckerImpl::TryDesugarExternMemberUpdate(ASTContext& ctx, AssignExpr& ae)
{
    if (ae.isCompound) {
        return false;
    }
    auto ma = DynamicCast<MemberAccess*>(ae.leftValue.get());
    if (ma == nullptr || ma->baseExpr == nullptr) {
        return false;
    }
    if (!Ty::IsTyCorrect(ma->baseExpr->GetTy()) || !TypeIsExtern(ma->baseExpr->GetTy())) {
        return false;
    }
    if (auto baseRef = DynamicCast<RefExpr*>(ma->baseExpr.get()); baseRef && baseRef->isThis) {
        // this corresponds to the `this.payload = payload` expression in the core library
        // which is a normal field assignment
        CJC_ASSERT(ma->field == "payload");
        return false;
    }

    auto typecheck = [this, &ctx](Ptr<Node> node) {
        auto ty = Synthesize({ctx, SynPos::EXPR_ARG}, node);
        ReplaceIdealTy(*node);
        return ty;
    };
    Ptr<Ty> rightTy = typecheck(ae.rightExpr.get());
    if (!Ty::IsTyCorrect(rightTy)) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }
    rightTy = ae.rightExpr->GetTy();

    auto sourceExternTy = ma->baseExpr->GetTy();
    auto info = ResolveExternRuntime(sourceExternTy);

    Ptr<FuncDecl> memberUpdateDecl = nullptr;
    auto memberUpdate = CreateRuntimeMemberAccess(ae, info, "memberUpdate", memberUpdateDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(Ptr(ma->baseExpr.get()))));
    args.emplace_back(CreateExternDesugarArg(CreateStringLit(ma->field.Val())));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(Ptr(ae.rightExpr.get()))));

    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    auto call = CreateRuntimeCall(ae, info, std::move(memberUpdate), memberUpdateDecl, std::move(args), unitTy);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "memberUpdate" &&
            typeManager.IsSubtype(call->GetTy(), unitTy)));

    ae.SetTy(unitTy);
    call->SetTy(unitTy);
    ae.desugarExpr = std::move(call);
    return true;
}

// `e[idx] = v` -> `T.indexUpdate(idx, v)` for `e: Extern<T>`
bool TypeChecker::TypeCheckerImpl::TryDesugarExternIndexUpdate(ASTContext& ctx, AssignExpr& ae)
{
    if (ae.isCompound) {
        return false;
    }
    auto se = DynamicCast<SubscriptExpr*>(ae.leftValue.get());
    if (!se || !se->baseExpr || se->indexExprs.empty()) {
        return false;
    }

    auto typecheck = [this, &ctx](Ptr<Node> node) {
        auto ty = Synthesize({ctx, SynPos::EXPR_ARG}, node);
        ReplaceIdealTy(*node);
        return ty;
    };
    SetIsNotAlone(*se->baseExpr);
    Ptr<Ty> sourceExternTy = typecheck(se->baseExpr.get());
    for (auto& indexExpr : se->indexExprs) {
        (void)typecheck(indexExpr.get());
    }
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(sourceExternTy)) {
        return false;
    }
    if (!std::all_of(se->indexExprs.cbegin(), se->indexExprs.cend(),
            [](auto& indexExpr) { return indexExpr && Ty::IsTyCorrect(indexExpr->GetTy()); })) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }

    Ptr<Ty> rightTy = typecheck(ae.rightExpr.get());
    if (!Ty::IsTyCorrect(rightTy)) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }
    rightTy = ae.rightExpr->GetTy();

    auto info = ResolveExternRuntime(sourceExternTy);

    // Build `T.indexAccess(base, idx)` for every index except the last; the last index is handled by
    // `indexUpdate` below.
    auto createRuntimeAccess = [&](OwnedPtr<Expr> baseExpr, Expr& indexExpr) -> OwnedPtr<CallExpr> {
        Ptr<FuncDecl> indexAccessDecl = nullptr;
        auto indexAccess = CreateRuntimeMemberAccess(ae, info, "indexAccess", indexAccessDecl);

        std::vector<OwnedPtr<FuncArg>> args;
        args.emplace_back(CreateExternDesugarArg(std::move(baseExpr)));
        args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(Ptr(&indexExpr))));

        auto call =
            CreateRuntimeCall(ae, info, std::move(indexAccess), indexAccessDecl, std::move(args), sourceExternTy);
        call->SetTy(sourceExternTy);
        return call;
    };

    OwnedPtr<Expr> updateBaseExpr = CloneEffectiveExpr(Ptr(se->baseExpr.get()));
    for (size_t i = 0; i + 1 < se->indexExprs.size(); ++i) {
        updateBaseExpr = createRuntimeAccess(std::move(updateBaseExpr), *se->indexExprs[i]);
        if (!updateBaseExpr) {
            ae.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
    }

    Ptr<FuncDecl> indexUpdateDecl = nullptr;
    auto indexUpdate = CreateRuntimeMemberAccess(ae, info, "indexUpdate", indexUpdateDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(updateBaseExpr)));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(Ptr(se->indexExprs.back().get()))));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(Ptr(ae.rightExpr.get()))));

    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    auto call = CreateRuntimeCall(ae, info, std::move(indexUpdate), indexUpdateDecl, std::move(args), unitTy);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "indexUpdate" &&
            typeManager.IsSubtype(call->GetTy(), unitTy)));

    ae.SetTy(unitTy);
    call->SetTy(unitTy);
    ae.desugarExpr = std::move(call);
    return true;
}

// `e(args...)` -> `T.functionCall(e, argsArray)` for `e: Extern<T>`.
// `e.foo(args...)` -> `T.functionCall(T.memberAccess(e, "foo"), argsArray)` for `e: Extern<T>`.
// More generally, the callee `ce.baseFunc` is treated as a value: if its type is `Extern<T>`,
// then the whole call is desugared into `T.functionCall(callee, [args...])`. Any dynamic member
// or index accesses contained in the callee are desugared by their own rules when the callee is
// synthesized, so e.g. `q.w.e.f1.f2(args)` becomes
// `T.functionCall(T.memberAccess(T.memberAccess(q.w.e, "f1"), "f2"), [args...])`.
bool TypeChecker::TypeCheckerImpl::TryDesugarFunctionCall(ASTContext& ctx, Ptr<Ty> target, CallExpr& ce)
{
    (void)target;
    if (!ce.baseFunc) {
        return false;
    }
    Ptr<Expr> base = ce.baseFunc.get();

    // Determine the type of the callee interpreted as a value. The callee has not been synthesized
    // yet at this point, so we synthesize it ourselves. Diagnostics are suppressed during this
    // probe so that a regular (non-extern) call is not affected: if the callee turns out not to be
    // `Extern<T>`, we discard the probe results and let the normal call-checking path run cleanly.
    Ptr<Ty> baseTy = base->GetTy();
    if (!Ty::IsTyCorrect(baseTy) || !TypeIsExtern(baseTy)) {
        auto ds = DiagSuppressor(diag);
        Synthesize({ctx, SynPos::EXPR_ARG}, base);
        ReplaceIdealTy(*base);
        baseTy = base->GetTy();
        if (!Ty::IsTyCorrect(baseTy) || !TypeIsExtern(baseTy)) {
            ctx.ClearTypeCheckCache(*base);
            return false;
        }
    }

    // A reference to a type (e.g. the constructor call `Extern<T>(payload)`) is not a value call and
    // must be handled by the regular call-checking path.
    if (auto tgt = base->GetTarget(); tgt && tgt->IsTypeDecl()) {
        ctx.ClearTypeCheckCache(*base);
        return false;
    }

    auto sourceExternTy = baseTy;
    auto info = ResolveExternRuntime(sourceExternTy);

    // Synthesize the user-provided arguments (so that nested extern expressions get desugared too)
    // and collect them as the elements of the `Array<Any>` passed to `functionCall`.
    std::vector<OwnedPtr<Expr>> elements;
    for (auto& arg : ce.args) {
        if (!arg || !arg->expr) {
            ce.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
        Synthesize({ctx, SynPos::EXPR_ARG}, arg->expr.get());
        ReplaceIdealTy(*arg->expr);
        if (!Ty::IsTyCorrect(arg->expr->GetTy())) {
            ce.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
        elements.emplace_back(CloneEffectiveExpr(arg->expr.get()));
    }

    // Build the `Array<Any>` literal holding the arguments.
    auto arrayDecl = importManager.GetCoreDecl<StructDecl>("Array");
    CJC_ASSERT(arrayDecl);
    auto arrayTy = typeManager.GetStructTy(*arrayDecl, {typeManager.GetAnyTy()});
    auto arrayLit = CreateArrayLit(std::move(elements), arrayTy);
    AddArrayLitConstructor(*arrayLit);
    arrayLit->EnableAttr(Attribute::COMPILER_ADD, Attribute::EXTERN_DESUGAR);
    arrayLit->curFile = ce.curFile;
    CopyBasicInfo(&ce, arrayLit.get());

    // Build `T.functionCall(callee, argsArray)`.
    Ptr<FuncDecl> functionCallDecl = nullptr;
    auto functionCall = CreateRuntimeMemberAccess(ce, info, "functionCall", functionCallDecl);

    OwnedPtr<Expr> calleeExpr = CloneEffectiveExpr(base);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(calleeExpr), sourceExternTy));
    args.emplace_back(CreateExternDesugarArg(std::move(arrayLit), arrayTy));

    auto call =
        CreateRuntimeCall(ce, info, std::move(functionCall), functionCallDecl, std::move(args), sourceExternTy);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "functionCall" &&
            typeManager.IsSubtype(call->GetTy(), sourceExternTy)));

    ce.SetTy(sourceExternTy);
    call->SetTy(sourceExternTy);
    ce.desugarExpr = std::move(call);
    return true;
}

bool TypeChecker::TypeCheckerImpl::TypeIsExtern(Ptr<Ty> ty)
{
    // Extern declaration always exists
    auto externDecl = importManager.GetCoreDecl<StructDecl>("Extern");
    CJC_ASSERT(externDecl);

    // Check if the type of the actual target is valid, and is Extern. If not, externification is unnecessary
    auto structTy = DynamicCast<StructTy*>(ty);
    return structTy && structTy->declPtr == externDecl && structTy->typeArgs.size() == 1;
}

Ptr<FuncDecl> TypeChecker::TypeCheckerImpl::GetRuntimeFuncDecl(const std::string& name)
{
    auto runtimeDecl = importManager.GetCoreDecl<InterfaceDecl>("Runtime");
    CJC_ASSERT(runtimeDecl);
    for (auto& member : runtimeDecl->GetMemberDecls()) {
        if (member && member->identifier == name) {
            return DynamicCast<FuncDecl*>(member.get());
        }
    }
    return nullptr;
}

TypeChecker::TypeCheckerImpl::ExternRuntimeInfo TypeChecker::TypeCheckerImpl::ResolveExternRuntime(Ptr<Ty> externTy)
{
    CJC_ASSERT(externTy && externTy->typeArgs.size() == 1);
    ExternRuntimeInfo info;
    info.runtimeTy = externTy->typeArgs[0];
    CJC_ASSERT(Ty::IsTyCorrect(info.runtimeTy));
    if (auto genericsTy = DynamicCast<GenericsTy*>(info.runtimeTy); genericsTy) {
        info.isGeneric = true;
        info.runtimeDecl = genericsTy->decl;
    } else {
        info.runtimeDecl = Ty::GetDeclPtrOfTy<Decl>(info.runtimeTy);
    }
    CJC_ASSERT(info.runtimeDecl);
    return info;
}

OwnedPtr<RefExpr> TypeChecker::TypeCheckerImpl::CreateExternRuntimeRef(
    const Node& srcNode, const ExternRuntimeInfo& info)
{
    auto runtimeRef = CreateRefExpr(*info.runtimeDecl);
    runtimeRef->isAlone = false;
    runtimeRef->SetTy(info.runtimeTy);
    runtimeRef->EnableAttr(Attribute::COMPILER_ADD, Attribute::EXTERN_DESUGAR);
    CopyBasicInfo(&srcNode, runtimeRef.get());
    return runtimeRef;
}

OwnedPtr<MemberAccess> TypeChecker::TypeCheckerImpl::CreateRuntimeMemberAccess(
    const Node& srcNode, const ExternRuntimeInfo& info, const std::string& runtimeFuncName, Ptr<FuncDecl>& outDecl)
{
    auto runtimeRef = CreateExternRuntimeRef(srcNode, info);

    OwnedPtr<MemberAccess> member;
    Ptr<FuncDecl> decl = nullptr;
    if (info.isGeneric) {
        member = MakeOwned<MemberAccess>();
        member->baseExpr = std::move(runtimeRef);
        member->field = runtimeFuncName;
        decl = GetRuntimeFuncDecl(runtimeFuncName);
        auto runtimeInterfaceDecl = importManager.GetCoreDecl<InterfaceDecl>("Runtime");
        CJC_ASSERT(runtimeInterfaceDecl);
        CJC_ASSERT(decl);
        auto typeMapping = GenerateTypeMapping(*runtimeInterfaceDecl, {info.runtimeTy});
        member->SetTy(typeManager.GetInstantiatedTy(decl->GetTy(), typeMapping));
    } else {
        member = CreateMemberAccess(std::move(runtimeRef), runtimeFuncName);
        decl = DynamicCast<FuncDecl*>(member->target);
    }
    member->isAlone = false;
    member->EnableAttr(Attribute::COMPILER_ADD, Attribute::EXTERN_DESUGAR);
    CopyBasicInfo(&srcNode, member.get());
    CJC_ASSERT(decl);
    member->target = decl;
    member->targets.emplace_back(decl);
    outDecl = decl;
    return member;
}

OwnedPtr<FuncArg> TypeChecker::TypeCheckerImpl::CreateExternDesugarArg(OwnedPtr<Expr> expr, Ptr<Ty> ty)
{
    if (ty == nullptr) {
        ty = expr->GetTy();
    }
    auto arg = CreateFuncArg(std::move(expr));
    arg->SetTy(ty);
    arg->EnableAttr(Attribute::EXTERN_DESUGAR);
    CJC_NULLPTR_CHECK(arg->expr);
    arg->expr->SetTy(ty);
    arg->expr->EnableAttr(Attribute::EXTERN_DESUGAR);
    return arg;
}

OwnedPtr<CallExpr> TypeChecker::TypeCheckerImpl::CreateRuntimeCall(const Expr& srcNode, const ExternRuntimeInfo& info,
    OwnedPtr<MemberAccess> member, Ptr<FuncDecl> decl, std::vector<OwnedPtr<FuncArg>> args, Ptr<Ty> retTy)
{
    auto callTarget = info.isGeneric ? nullptr : decl;
    auto call = CreateCallExpr(std::move(member), std::move(args), callTarget, retTy, CallKind::CALL_DECLARED_FUNCTION);
    CopyBasicInfo(&srcNode, call.get());
    call->sourceExpr = const_cast<Expr*>(&srcNode);
    call->EnableAttr(Attribute::COMPILER_ADD, Attribute::EXTERN_DESUGAR);
    call->resolvedFunction = decl;
    return call;
}

OwnedPtr<LitConstExpr> TypeChecker::TypeCheckerImpl::CreateStringLit(const std::string& value)
{
    auto stringDecl = importManager.GetCoreDecl<StructDecl>("String");
    CJC_ASSERT(stringDecl);
    return CreateLitConstExpr(LitConstKind::STRING, value, stringDecl->GetTy(), true);
}

} // namespace Cangjie