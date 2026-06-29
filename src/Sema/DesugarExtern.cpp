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

#include "DiagSuppressor.h"
#include "Diags.h"
#include "TypeCheckUtil.h"
#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"

namespace Cangjie {
using namespace TypeCheckUtil;
using namespace AST;
using namespace Sema;

OwnedPtr<Expr> CloneEffectiveExpr(Expr& expr)
{
    OwnedPtr<Expr> cloned;
    if (expr.desugarExpr) {
        cloned = ASTCloner::Clone(Ptr(expr.desugarExpr.get()));
    } else {
        cloned = ASTCloner::Clone(Ptr(&expr));
    }
    if (cloned) {
        cloned->EnableAttr(Attribute::EXTERN_DESUGAR);
    }
    return cloned;
}

OwnedPtr<LitConstExpr> CreateStringLit(ImportManager& importManager, const std::string& value)
{
    auto stringDecl = importManager.GetCoreDecl<StructDecl>("String");
    CJC_ASSERT(stringDecl);
    return CreateLitConstExpr(LitConstKind::STRING, value, stringDecl->GetTy(), true);
}

// `x` -> `T.toExtern(x)` in a context of type `Extern<T>`
bool TypeChecker::TypeCheckerImpl::CoerceToExtern(ASTContext& ctx, Ty& targetTy, Expr& nodeExpr)
{
    CJC_ASSERT(TypeIsExtern(targetTy));
    CJC_ASSERT(targetTy.typeArgs.size() == 1);

    // Synthesize the source expression first so that we know its type `R` and so that any nested
    // extern expressions it contains get desugared.
    Synthesize({ctx, SynPos::EXPR_ARG}, &nodeExpr);
    ReplaceIdealTy(nodeExpr);

    auto sourceTy = nodeExpr.GetTy();
    if (!Ty::IsTyCorrect(sourceTy)) {
        nodeExpr.SetTy(TypeManager::GetInvalidTy());
        return false;
    }

    // `R` is already a subtype of `Extern<T>`, so no coercion is necessary.
    if (typeManager.IsSubtype(sourceTy, &targetTy)) {
        return true;
    }

    // Runtime type `T` of `Extern<T>`.
    auto info = ResolveExternRuntime(targetTy);

    // Grab the effective inner expression (the desugared form if `nodeExpr` was itself desugared)
    // as the argument to `toExtern`.
    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(nodeExpr), sourceTy));

    // Build `T.toExtern`. Unlike the other runtime members, `toExtern<R>` is a generic method, so we
    // pin its type argument to `R` and give the member access the instantiated function type
    // `(R) -> Extern<T>`.
    Ptr<FuncDecl> toExternDecl = nullptr;
    auto toExtern = CreateRuntimeMemberAccess(nodeExpr, info, "toExtern", toExternDecl);
    toExtern->instTys.clear();
    toExtern->instTys.emplace_back(sourceTy);
    toExtern->SetTy(typeManager.GetFunctionTy({sourceTy}, &targetTy));

    auto call = CreateRuntimeCall(nodeExpr, info, std::move(toExtern), *toExternDecl, std::move(args), targetTy);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "toExtern" &&
            typeManager.IsSubtype(call->GetTy(), &targetTy)));

    nodeExpr.SetTy(&targetTy);
    call->SetTy(&targetTy);
    nodeExpr.desugarExpr = std::move(call);
    return true;
}

// `e.foo` -> `T.memberAccess(e, "foo")` for `e: Extern<T>`.
// Chains such as `e1.f1.f2` and `a.b.e1.f1.f2` (where only `a.b.e1` is `Extern<T>`) are handled
// automatically: the inner member access `...f1` is synthesized first and, being itself an extern
// member access, is replaced by its own `T.memberAccess(...)` desugaring. `CloneEffectiveExpr` then
// picks up that desugared form as the base, producing nested `T.memberAccess` calls.
// `e.foo = v` (left value) is handled by `TryDesugarExternMemberUpdate`, and `e.foo(args...)` by
// `TryDesugarFunctionCall`, so both cases are deferred here.
bool TypeChecker::TypeCheckerImpl::TryDesugarExternMemberAccess(MemberAccess& ma)
{
    CJC_NULLPTR_CHECK(ma.baseExpr);
    auto sourceExternTy = ma.baseExpr->GetTy();
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(*sourceExternTy)) {
        return false;
    }
    // As a left value, the member access is the target of an assignment and is desugared into
    // `T.memberUpdate(...)` by `TryDesugarExternMemberUpdate`.
    if (ma.TestAttr(Attribute::LEFT_VALUE)) {
        if (auto baseRef = DynamicCast<RefExpr*>(ma.baseExpr.get()); baseRef && baseRef->isThis) {
            // `this.payload` (part of `this.payload = payload`) in the core library is a normal field
            // access on `Extern` itself, not a dynamic member access, and must be left alone.
            CJC_ASSERT(ma.field == "payload");
            return false;
        }
        ma.SetTy(sourceExternTy);
        return true;
    }
    // As the callee of a call, the member access is desugared together with the call into
    // `T.functionCall(T.memberAccess(...), [args...])` by `TryDesugarFunctionCall`.
    if (ma.callOrPattern) {
        return false;
    }

    auto info = ResolveExternRuntime(*sourceExternTy);

    Ptr<FuncDecl> memberAccessDecl = nullptr;
    auto memberAccess = CreateRuntimeMemberAccess(ma, info, "memberAccess", memberAccessDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*ma.baseExpr)));
    args.emplace_back(CreateExternDesugarArg(CreateStringLit(importManager, ma.field.Val())));

    auto call =
        CreateRuntimeCall(ma, info, std::move(memberAccess), *memberAccessDecl, std::move(args), *sourceExternTy);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "memberAccess" &&
            typeManager.IsSubtype(call->GetTy(), sourceExternTy)));

    ma.SetTy(sourceExternTy);
    call->SetTy(sourceExternTy);
    ma.desugarExpr = std::move(call);
    return true;
}

// `e[idx]` -> `T.indexAccess(e, idx)` for `e: Extern<T>`.
// The base and indices have already been synthesized by `ChkSubscriptExpr` before this is called,
// so any nested extern member/index accesses contained in the base are already desugared and are
// picked up here via `CloneEffectiveExpr`. Chains such as `a.b.e[idx]` (where only `a.b.e` is
// `Extern<T>`) and `e[i1][i2]` therefore desugar naturally. A subscript that is the callee of a
// call (`e[idx](args...)`) or the target of an assignment (`e[idx] = v`) is handled by
// `TryDesugarFunctionCall` / `TryDesugarExternIndexUpdate` respectively: in those cases this
// desugaring still runs for the read part and the enclosing rule reuses the desugared form.
// Multiple indices `e[i1, i2, ...]` are chained into nested `indexAccess` calls, matching the
// handling in `TryDesugarExternIndexUpdate`.
bool TypeChecker::TypeCheckerImpl::TryDesugarExternIndexAccess(SubscriptExpr& se)
{
    if (!se.baseExpr || se.indexExprs.empty()) {
        return false;
    }
    auto sourceExternTy = se.baseExpr->GetTy();
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(*sourceExternTy)) {
        return false;
    }
    if (!std::all_of(se.indexExprs.cbegin(), se.indexExprs.cend(),
            [](auto& indexExpr) { return indexExpr && Ty::IsTyCorrect(indexExpr->GetTy()); })) {
        se.SetTy(TypeManager::GetInvalidTy());
        return true;
    }

    // The base and indices were synthesized by `ChkSubscriptExpr` without replacing ideal types
    // (e.g. the literal `0` in `e[0]` still carries an ideal integer type). Each index is passed to
    // `indexAccess` as an `Any` argument, so it must be given its concrete type before being cloned
    // into the desugared call; otherwise post-typecheck AST validation rejects the ideal-typed node.
    ReplaceIdealTy(*se.baseExpr);
    for (auto& indexExpr : se.indexExprs) {
        ReplaceIdealTy(*indexExpr);
    }

    auto info = ResolveExternRuntime(*sourceExternTy);

    // Build `T.indexAccess(base, idx)` once per index, chaining when there is more than one.
    OwnedPtr<Expr> accessExpr = CloneEffectiveExpr(*se.baseExpr);
    for (auto& indexExpr : se.indexExprs) {
        accessExpr = CreateRuntimeIndexAccess(se, info, std::move(accessExpr), *indexExpr, *sourceExternTy);
    }

    se.SetTy(sourceExternTy);
    accessExpr->SetTy(sourceExternTy);
    se.desugarExpr = std::move(accessExpr);
    return true;
}
// `e.foo = v` -> `T.memberUpdate(e, "foo", v)` for `e: Extern<T>`.
// Chains such as `e.f1.f2 = v` and `a.b.e.f1 = v` (where only the inner sub-expression is
// `Extern<T>`) are handled by the base member access: when the left value is synthesized by the
// caller (`SynAssignExpr`), its base expression (here `ma->baseExpr`) is synthesized first, so any
// nested extern member/index/call access it contains is already desugared. `CloneEffectiveExpr`
// then picks up that desugared form, producing e.g.
// `T.memberUpdate(T.memberAccess(e, "f1"), "f2", v)`. Bases such as `a().f1` or `b["k"].f1` rely on
// `TryDesugarFunctionCall` / `TryDesugarExternIndexAccess` having run during that same synthesis.
bool TypeChecker::TypeCheckerImpl::TryDesugarExternMemberUpdate(ASTContext& ctx, AssignExpr& ae)
{
    if (ae.isCompound) {
        return false;
    }
    auto ma = DynamicCast<MemberAccess*>(ae.leftValue.get());
    if (!ma || !ma->baseExpr) {
        return false;
    }

    // The left value (and therefore its base expression) has already been synthesized by
    // `SynAssignExpr` before reaching this point, so the base's type and any desugaring are
    // available without re-synthesizing here.
    auto sourceExternTy = ma->baseExpr->GetTy();
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(*sourceExternTy)) {
        return false;
    }
    if (auto baseRef = DynamicCast<RefExpr*>(ma->baseExpr.get()); baseRef && baseRef->isThis) {
        // `this.payload = payload` in the core library is a normal field assignment on `Extern`
        // itself, not a dynamic member update, and must be left alone.
        CJC_ASSERT(ma->field == "payload");
        return false;
    }

    // Synthesize the right-hand side value so that nested extern expressions get desugared, and pin
    // its ideal type before it is cloned as the `Any` argument to `memberUpdate`.
    Synthesize({ctx, SynPos::EXPR_ARG}, ae.rightExpr.get());
    ReplaceIdealTy(*ae.rightExpr);
    if (!Ty::IsTyCorrect(ae.rightExpr->GetTy())) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }

    auto info = ResolveExternRuntime(*sourceExternTy);

    Ptr<FuncDecl> memberUpdateDecl = nullptr;
    auto memberUpdate = CreateRuntimeMemberAccess(ae, info, "memberUpdate", memberUpdateDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*ma->baseExpr)));
    args.emplace_back(CreateExternDesugarArg(CreateStringLit(importManager, ma->field.Val())));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*ae.rightExpr)));

    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    auto call = CreateRuntimeCall(ae, info, std::move(memberUpdate), *memberUpdateDecl, std::move(args), *unitTy);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "memberUpdate" &&
            typeManager.IsSubtype(call->GetTy(), unitTy)));

    ae.SetTy(unitTy);
    call->SetTy(unitTy);
    ae.desugarExpr = std::move(call);
    return true;
}

// `e[idx] = v` -> `T.indexUpdate(e, idx, v)` for `e: Extern<T>`.
// Unlike `TryDesugarExternMemberUpdate`, this runs from `SynAssignExpr` *before* the left value is
// type-checked, so the subscript base and indices are synthesized here. The base type is probed
// with diagnostics suppressed: if it is not `Extern<T>`, the probe is discarded and `false` is
// returned so the regular subscript-assignment / operator-overload path runs cleanly. Once the base
// is known to be `Extern<T>` we commit, and the indices and right-hand side are synthesized with
// diagnostics enabled.
// Chains such as `a.b.e[idx] = v`, `e.f1[idx] = v`, `a()[idx] = v` and `b["k"][idx] = v` are handled
// by the base: synthesizing it desugars any nested extern member/index/call access first, and
// `CloneEffectiveExpr` then picks up that desugared form as the receiver of `indexUpdate`
// (e.g. `e.f1[idx] = v` becomes `T.indexUpdate(T.memberAccess(e, "f1"), idx, v)`).
// Multiple indices `e[i1, ..., iN] = v` desugar into
// `T.indexUpdate(T.indexAccess(...T.indexAccess(e, i1)..., i(N-1)), iN, v)`, matching the read-side
// chaining in `TryDesugarExternIndexAccess`.
bool TypeChecker::TypeCheckerImpl::TryDesugarExternIndexUpdate(ASTContext& ctx, AssignExpr& ae)
{
    if (ae.isCompound) {
        return false;
    }
    auto se = DynamicCast<SubscriptExpr*>(ae.leftValue.get());
    if (!se || !se->baseExpr || se->indexExprs.empty()) {
        return false;
    }

    // The left value has not been synthesized yet, so determine the base type ourselves. Diagnostics
    // are suppressed during this probe so that a regular (non-extern) subscript assignment is not
    // affected: if the base turns out not to be `Extern<T>`, the probe results are discarded and the
    // normal subscript-assignment path runs cleanly.
    SetIsNotAlone(*se->baseExpr);
    Ptr<Ty> sourceExternTy = se->baseExpr->GetTy();
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(*sourceExternTy)) {
        auto ds = DiagSuppressor(diag);
        Synthesize({ctx, SynPos::EXPR_ARG}, se->baseExpr.get());
        ReplaceIdealTy(*se->baseExpr);
        sourceExternTy = se->baseExpr->GetTy();
        if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(*sourceExternTy)) {
            ctx.ClearTypeCheckCache(*se->baseExpr);
            return false;
        }
    }

    // Committed to the extern desugaring. Synthesize the indices and the right-hand side value (so
    // that nested extern expressions get desugared) and pin their ideal types before they are cloned
    // as the `Any` arguments to `indexAccess` / `indexUpdate`; otherwise post-typecheck AST
    // validation rejects the ideal-typed nodes.
    for (auto& indexExpr : se->indexExprs) {
        if (!indexExpr) {
            ae.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
        Synthesize({ctx, SynPos::EXPR_ARG}, indexExpr.get());
        ReplaceIdealTy(*indexExpr);
    }
    Synthesize({ctx, SynPos::EXPR_ARG}, ae.rightExpr.get());
    ReplaceIdealTy(*ae.rightExpr);
    ReplaceIdealTy(*se->baseExpr);

    bool valid = Ty::IsTyCorrect(ae.rightExpr->GetTy()) &&
        std::all_of(se->indexExprs.cbegin(), se->indexExprs.cend(),
            [](auto& indexExpr) { return indexExpr && Ty::IsTyCorrect(indexExpr->GetTy()); });
    if (!valid) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }

    auto info = ResolveExternRuntime(*sourceExternTy);

    // Receiver of the final `indexUpdate`: the base, with all but the last index applied via
    // `T.indexAccess(base, idx)` (the read part of the chain).
    OwnedPtr<Expr> receiver = CloneEffectiveExpr(*se->baseExpr);
    for (size_t i = 0; i + 1 < se->indexExprs.size(); ++i) {
        receiver = CreateRuntimeIndexAccess(*se, info, std::move(receiver), *se->indexExprs[i], *sourceExternTy);
    }

    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);

    Ptr<FuncDecl> indexUpdateDecl = nullptr;
    auto indexUpdate = CreateRuntimeMemberAccess(ae, info, "indexUpdate", indexUpdateDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(receiver)));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*se->indexExprs.back())));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*ae.rightExpr)));

    auto call = CreateRuntimeCall(ae, info, std::move(indexUpdate), *indexUpdateDecl, std::move(args), *unitTy);
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
bool TypeChecker::TypeCheckerImpl::TryDesugarFunctionCall(ASTContext& ctx, CallExpr& ce)
{
    if (!ce.baseFunc) {
        return false;
    }
    Ptr<Expr> base = ce.baseFunc.get();

    // Determine the type of the callee interpreted as a value. The callee has not been synthesized
    // yet at this point, so we synthesize it ourselves. Diagnostics are suppressed during this
    // probe so that a regular (non-extern) call is not affected: if the callee turns out not to be
    // `Extern<T>`, we discard the probe results and let the normal call-checking path run cleanly.
    Ptr<Ty> baseTy = base->GetTy();
    if (!Ty::IsTyCorrect(baseTy) || !TypeIsExtern(*baseTy)) {
        auto ds = DiagSuppressor(diag);
        Synthesize({ctx, SynPos::EXPR_ARG}, base);
        ReplaceIdealTy(*base);
        baseTy = base->GetTy();
        if (!Ty::IsTyCorrect(baseTy) || !TypeIsExtern(*baseTy)) {
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
    auto info = ResolveExternRuntime(*sourceExternTy);

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
        elements.emplace_back(CloneEffectiveExpr(*arg->expr));
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

    OwnedPtr<Expr> calleeExpr = CloneEffectiveExpr(*base);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(calleeExpr), sourceExternTy));
    args.emplace_back(CreateExternDesugarArg(std::move(arrayLit), arrayTy));

    auto call =
        CreateRuntimeCall(ce, info, std::move(functionCall), *functionCallDecl, std::move(args), *sourceExternTy);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "functionCall" &&
            typeManager.IsSubtype(call->GetTy(), sourceExternTy)));

    ce.SetTy(sourceExternTy);
    call->SetTy(sourceExternTy);
    ce.desugarExpr = std::move(call);
    return true;
}

bool TypeChecker::TypeCheckerImpl::TypeIsExtern(Ty& ty)
{
    // Extern declaration always exists
    auto externDecl = importManager.GetCoreDecl<StructDecl>("Extern");
    CJC_ASSERT(externDecl);

    // Check if the type of the actual target is valid, and is Extern. If not, externification is unnecessary
    auto structTy = DynamicCast<StructTy*>(&ty);
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

TypeChecker::TypeCheckerImpl::ExternRuntimeInfo TypeChecker::TypeCheckerImpl::ResolveExternRuntime(Ty& externTy)
{
    CJC_ASSERT(externTy.typeArgs.size() == 1);
    ExternRuntimeInfo info;
    info.runtimeTy = externTy.typeArgs[0];
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
    OwnedPtr<MemberAccess> member, FuncDecl& decl, std::vector<OwnedPtr<FuncArg>> args, Ty& retTy)
{
    Ptr<FuncDecl> callTarget = info.isGeneric ? nullptr : &decl;
    auto call =
        CreateCallExpr(std::move(member), std::move(args), callTarget, &retTy, CallKind::CALL_DECLARED_FUNCTION);
    CopyBasicInfo(&srcNode, call.get());
    call->sourceExpr = const_cast<Expr*>(&srcNode);
    call->EnableAttr(Attribute::COMPILER_ADD, Attribute::EXTERN_DESUGAR);
    call->resolvedFunction = &decl;
    return call;
}

OwnedPtr<CallExpr> TypeChecker::TypeCheckerImpl::CreateRuntimeIndexAccess(
    const Expr& srcNode, const ExternRuntimeInfo& info, OwnedPtr<Expr> baseExpr, Expr& indexExpr, Ty& ty)
{
    Ptr<FuncDecl> indexAccessDecl = nullptr;
    auto indexAccess = CreateRuntimeMemberAccess(srcNode, info, "indexAccess", indexAccessDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(baseExpr)));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(indexExpr)));

    auto call = CreateRuntimeCall(srcNode, info, std::move(indexAccess), *indexAccessDecl, std::move(args), ty);
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "indexAccess" &&
            typeManager.IsSubtype(call->GetTy(), &ty)));
    call->SetTy(&ty);
    return call;
}

} // namespace Cangjie