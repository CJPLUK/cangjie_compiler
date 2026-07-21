// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "DiagSuppressor.h"
#include "Diags.h"
#include "TypeCheckUtil.h"
#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Walker.h"

namespace Cangjie {
using namespace TypeCheckUtil;
using namespace AST;
using namespace Sema;

static const std::string EXTERN_TO_EXTERN = "toExtern";
static const std::string EXTERN_FROM_EXTERN = "fromExtern";
static const std::string EXTERN_MEMBER_ACCESS = "memberAccess";
static const std::string EXTERN_MEMBER_UPDATE = "memberUpdate";
static const std::string EXTERN_INDEXED_ACCESS = "indexedAccess";
static const std::string EXTERN_INDEXED_UPDATE = "indexedUpdate";
static const std::string EXTERN_FUNCTION_CALL = "functionCall";
static const std::string EXTERN_PAYLOAD_FIELD = "payload";
static const std::string EXTERN_TYPE_EXTERN = "Extern";
static const std::string EXTERN_TYPE_RUNTIME = "ForeignRuntime";

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
    auto stringDecl = importManager.GetCoreDecl<StructDecl>(STD_LIB_STRING);
    CJC_ASSERT(stringDecl);
    return CreateLitConstExpr(LitConstKind::STRING, value, stringDecl->GetTy(), true);
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::ProbeExternBaseTy(ASTContext& ctx, Expr& expr)
{
    Ptr<Ty> ty = expr.GetTy();
    if (Ty::IsTyCorrect(ty)) {
        return TypeIsExtern(*ty) ? ty : nullptr;
    }
    // Diagnostics are suppressed during the probe so that a regular (non-extern) expression is not
    // affected: if it turns out not to be `Extern<T>`, the probe results are discarded and the
    // normal path runs cleanly.
    auto ds = DiagSuppressor(diag);
    Synthesize({ctx, SynPos::EXPR_ARG}, &expr);
    ReplaceIdealTy(expr);
    ty = expr.GetTy();
    if (!Ty::IsTyCorrect(ty) || !TypeIsExtern(*ty)) {
        ctx.ClearTypeCheckCache(expr);
        return nullptr;
    }
    return ty;
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

    // Coercion required. Assign the target `Extern<T>` type now and tag the node; the
    // `T.toExtern<R>(x)` lowering is deferred to `DesugarExternCoerce`. Record the source type `R`
    // (needed to instantiate `toExtern<R>`) because assigning `Extern<T>` here overwrites the node's
    // own type.
    externCoerceSourceTy[&nodeExpr] = sourceTy;
    nodeExpr.SetTy(&targetTy);
    nodeExpr.EnableAttr(Attribute::EXTERN_PENDING_COERCE);
    return true;
}

void TypeChecker::TypeCheckerImpl::DesugarExternCoerce(Expr& nodeExpr, Ptr<Ty> sourceTy)
{
    auto targetTy = nodeExpr.GetTy();
    CJC_ASSERT(Ty::IsTyCorrect(targetTy) && TypeIsExtern(*targetTy));
    CJC_ASSERT(Ty::IsTyCorrect(sourceTy));

    // Runtime type `T` of `Extern<T>`.
    auto info = ResolveExternRuntime(*targetTy);

    // Grab the effective inner expression (the desugared form if `nodeExpr` was itself desugared, e.g.
    // a nested extern access built earlier in this bottom-up pass) as the argument to `toExtern`.
    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(nodeExpr), sourceTy));

    // Build `T.toExtern`. Unlike the other runtime members, `toExtern<R>` is a generic method, so we
    // pin its type argument to `R` and give the member access the instantiated function type
    // `(R) -> Extern<T>`.
    Ptr<FuncDecl> toExternDecl = nullptr;
    auto toExtern = CreateRuntimeMemberAccess(nodeExpr, info, EXTERN_TO_EXTERN, toExternDecl);
    toExtern->instTys.clear();
    toExtern->instTys.emplace_back(sourceTy);
    toExtern->SetTy(typeManager.GetFunctionTy({sourceTy}, targetTy));

    auto call = CreateRuntimeCall(nodeExpr, info, std::move(toExtern), *toExternDecl, std::move(args), *targetTy);

    call->SetTy(targetTy);
    nodeExpr.desugarExpr = std::move(call);
}

// `e.foo` -> `T.memberAccess(e, "foo")` for `e: Extern<T>`.
// Chains such as `e1.f1.f2` and `a.b.e1.f1.f2` (where only `a.b.e1` is `Extern<T>`) are handled
// automatically: the inner member access `...f1` is synthesized first and, being itself an extern
// member access, is replaced by its own `T.memberAccess(...)` desugaring. `CloneEffectiveExpr` then
// picks up that desugared form as the base, producing nested `T.memberAccess` calls.
// `e.foo = v` (left value) is the target of an assignment and is desugared into `T.memberUpdate(...)`
// by `TypeCheckExternMemberUpdate`, so that case is deferred here.
bool TypeChecker::TypeCheckerImpl::TypeCheckExternMemberAccess(ASTContext& ctx, MemberAccess& ma)
{
    CJC_NULLPTR_CHECK(ma.baseExpr);
    auto sourceExternTy = ma.baseExpr->GetTy();
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(*sourceExternTy)) {
        return false;
    }
    // A static member access on the `Extern` type itself (e.g. `Extern<T>.getPayload(...)`) is a real
    // static reference, not a dynamic member access on an `Extern` value, and must not be desugared:
    // the base is a type, so desugaring would leave the `Extern<T>` type reference as a runtime value
    // argument. Such an access is identified by its base referring to a type declaration.
    if (auto baseTarget = ma.baseExpr->GetTarget(); baseTarget && baseTarget->IsTypeDecl()) {
        return false;
    }
    // As a left value, the member access is the target of an assignment and is lowered into
    // `T.memberUpdate(...)` by the enclosing assignment's desugaring. Only assign the type here.
    if (ma.TestAttr(Attribute::LEFT_VALUE)) {
        if (auto baseRef = DynamicCast<RefExpr*>(ma.baseExpr.get()); baseRef && baseRef->isThis) {
            // `this.payload = payload` in `Extern`'s constructor is a normal field write on `Extern`
            // itself, not a dynamic member update, and must be left alone.
            CJC_ASSERT(ma.field == EXTERN_PAYLOAD_FIELD);
            return false;
        }
        ma.SetTy(sourceExternTy);
        return true;
    }
    // `e.payload` read in the body of the static `getPayload` helper of the core `Extern` struct is a
    // normal read of the private `payload` field, not a dynamic member access, and must be left alone
    // -- mirroring the `this.payload` handling above. It is identified by the function the expression
    // belongs to (obtained from the current scope) being a member of the core `Extern` struct.
    if (ma.field == EXTERN_PAYLOAD_FIELD) {
        auto curFuncBody = GetCurFuncBody(ctx, ma.scopeName);
        auto externDecl = importManager.GetCoreDecl<StructDecl>(EXTERN_TYPE_EXTERN);
        if (curFuncBody && curFuncBody->funcDecl && externDecl &&
            curFuncBody->funcDecl->outerDecl.get() == externDecl) {
            return false;
        }
    }
    // Dynamic extern member read. Assign the `Extern<T>` type now and tag the node; the
    // `T.memberAccess(e, "foo")` lowering is deferred to `DesugarExternMemberAccess`. When the access
    // is the callee of a call (`e.foo(args...)`), the enclosing call is likewise typed as an extern
    // value call by `TypeCheckExternFunctionCall` and lowered into
    // `T.functionCall(T.memberAccess(e, "foo"), [args...])`.
    ma.SetTy(sourceExternTy);
    ma.EnableAttr(Attribute::EXTERN_PENDING_DESUGAR);
    return true;
}

void TypeChecker::TypeCheckerImpl::DesugarExternMemberAccess(MemberAccess& ma)
{
    CJC_NULLPTR_CHECK(ma.baseExpr);
    auto sourceExternTy = ma.baseExpr->GetTy();
    CJC_ASSERT(Ty::IsTyCorrect(sourceExternTy) && TypeIsExtern(*sourceExternTy));
    auto info = ResolveExternRuntime(*sourceExternTy);

    Ptr<FuncDecl> memberAccessDecl = nullptr;
    auto memberAccess = CreateRuntimeMemberAccess(ma, info, EXTERN_MEMBER_ACCESS, memberAccessDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*ma.baseExpr)));
    args.emplace_back(CreateExternDesugarArg(CreateStringLit(importManager, ma.field.Val())));

    auto call =
        CreateRuntimeCall(ma, info, std::move(memberAccess), *memberAccessDecl, std::move(args), *sourceExternTy);

    call->SetTy(sourceExternTy);
    ma.desugarExpr = std::move(call);
}

// `e[idx]` -> `T.indexedAccess(e, idx)` for `e: Extern<T>`.
// The base and indices have already been synthesized by `ChkSubscriptExpr` before this is called,
// so any nested extern member/index accesses contained in the base are already desugared and are
// picked up here via `CloneEffectiveExpr`. Chains such as `a.b.e[idx]` (where only `a.b.e` is
// `Extern<T>`) and `e[i1][i2]` therefore desugar naturally. A subscript that is the callee of a
// call (`e[idx](args...)`) or the target of an assignment (`e[idx] = v`) is handled by
// `TypeCheckExternFunctionCall` / `TypeCheckExternIndexUpdate` respectively: in those cases this
// desugaring still runs for the read part and the enclosing rule reuses the desugared form.
// Multiple indices `e[i1, i2, ...]` are chained into nested `indexedAccess` calls, matching the
// handling in `TypeCheckExternIndexUpdate`.
bool TypeChecker::TypeCheckerImpl::TypeCheckExternIndexAccess(SubscriptExpr& se)
{
    if (!se.baseExpr || se.indexExprs.empty()) {
        return false;
    }
    auto sourceExternTy = se.baseExpr->GetTy();
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(*sourceExternTy)) {
        return false;
    }
    auto checkIndexExpr = [](auto& indexExpr) { return indexExpr && Ty::IsTyCorrect(indexExpr->GetTy()); };
    if (!std::all_of(se.indexExprs.cbegin(), se.indexExprs.cend(), checkIndexExpr)) {
        se.SetTy(TypeManager::GetInvalidTy());
        return true;
    }
    // The base and indices were synthesized without replacing ideal types (e.g. the literal `0` in
    // `e[0]` still carries an ideal integer type). Each index becomes an `Any` argument to
    // `indexedAccess`, so pin its concrete type now; otherwise post-typecheck AST validation rejects
    // the ideal-typed node.
    ReplaceIdealTy(*se.baseExpr);
    for (auto& indexExpr : se.indexExprs) {
        ReplaceIdealTy(*indexExpr);
    }
    // Dynamic extern index read. Assign the type now and tag the node; the `T.indexedAccess(...)`
    // chain is built later by `DesugarExternIndexAccess`.
    se.SetTy(sourceExternTy);
    se.EnableAttr(Attribute::EXTERN_PENDING_DESUGAR);
    return true;
}

void TypeChecker::TypeCheckerImpl::DesugarExternIndexAccess(SubscriptExpr& se)
{
    CJC_NULLPTR_CHECK(se.baseExpr);
    auto sourceExternTy = se.baseExpr->GetTy();
    CJC_ASSERT(Ty::IsTyCorrect(sourceExternTy) && TypeIsExtern(*sourceExternTy));

    auto info = ResolveExternRuntime(*sourceExternTy);

    // Build `T.indexedAccess(base, idx)` once per index, chaining when there is more than one.
    OwnedPtr<Expr> accessExpr = CloneEffectiveExpr(*se.baseExpr);
    for (auto& indexExpr : se.indexExprs) {
        accessExpr = CreateRuntimeIndexAccess(se, info, std::move(accessExpr), *indexExpr, *sourceExternTy);
    }

    accessExpr->SetTy(sourceExternTy);
    se.desugarExpr = std::move(accessExpr);
}
// `e.foo = v` -> `T.memberUpdate(e, "foo", v)` for `e: Extern<T>`.
// Chains such as `e.f1.f2 = v` and `a.b.e.f1 = v` (where only the inner sub-expression is
// `Extern<T>`) are handled by the base member access: when the left value is synthesized by the
// caller (`SynAssignExpr`), its base expression (here `ma->baseExpr`) is synthesized first, so any
// nested extern member/index/call access it contains is already desugared. `CloneEffectiveExpr`
// then picks up that desugared form, producing e.g.
// `T.memberUpdate(T.memberAccess(e, "f1"), "f2", v)`. Bases such as `a().f1` or `b["k"].f1` rely on
// `TypeCheckExternFunctionCall` / `TypeCheckExternIndexAccess` having run during that same synthesis.
bool TypeChecker::TypeCheckerImpl::TypeCheckExternMemberUpdate(ASTContext& ctx, AssignExpr& ae)
{
    if (ae.isCompound) {
        return false;
    }
    auto ma = DynamicCast<MemberAccess*>(ae.leftValue.get());
    if (!ma || !ma->baseExpr) {
        return false;
    }

    // The left value (and therefore its base expression) has already been synthesized by
    // `SynAssignExpr` before reaching this point, so the base's type is available here.
    auto sourceExternTy = ma->baseExpr->GetTy();
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(*sourceExternTy)) {
        return false;
    }
    if (auto baseRef = DynamicCast<RefExpr*>(ma->baseExpr.get()); baseRef && baseRef->isThis) {
        // `this.payload = payload` in the core library is a normal field assignment on `Extern`
        // itself, not a dynamic member update, and must be left alone.
        CJC_ASSERT(ma->field == EXTERN_PAYLOAD_FIELD);
        return false;
    }

    // Synthesize the right-hand side value (so it is type-checked) and pin its ideal type.
    Synthesize({ctx, SynPos::EXPR_ARG}, ae.rightExpr);
    ReplaceIdealTy(*ae.rightExpr);
    if (!Ty::IsTyCorrect(ae.rightExpr->GetTy())) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }

    // Dynamic extern member update. Assign the `Unit` type now and tag the assignment; the
    // `T.memberUpdate(e, "foo", v)` lowering is deferred to `DesugarExternMemberUpdate`.
    ae.SetTy(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));
    ae.EnableAttr(Attribute::EXTERN_PENDING_DESUGAR);
    return true;
}

void TypeChecker::TypeCheckerImpl::DesugarExternMemberUpdate(AssignExpr& ae)
{
    auto ma = DynamicCast<MemberAccess*>(ae.leftValue.get());
    CJC_NULLPTR_CHECK(ma);
    CJC_NULLPTR_CHECK(ma->baseExpr);
    auto sourceExternTy = ma->baseExpr->GetTy();
    CJC_ASSERT(Ty::IsTyCorrect(sourceExternTy) && TypeIsExtern(*sourceExternTy));

    auto info = ResolveExternRuntime(*sourceExternTy);

    Ptr<FuncDecl> memberUpdateDecl = nullptr;
    auto memberUpdate = CreateRuntimeMemberAccess(ae, info, EXTERN_MEMBER_UPDATE, memberUpdateDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*ma->baseExpr)));
    args.emplace_back(CreateExternDesugarArg(CreateStringLit(importManager, ma->field.Val())));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*ae.rightExpr)));

    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    auto call = CreateRuntimeCall(ae, info, std::move(memberUpdate), *memberUpdateDecl, std::move(args), *unitTy);

    call->SetTy(unitTy);
    ae.desugarExpr = std::move(call);
}

// `e[idx] = v` -> `T.indexedUpdate(e, idx, v)` for `e: Extern<T>`.
// Unlike `TypeCheckExternMemberUpdate`, this runs from `SynAssignExpr` *before* the left value is
// type-checked, so the subscript base and indices are synthesized here. The base type is probed
// with diagnostics suppressed: if it is not `Extern<T>`, the probe is discarded and `false` is
// returned so the regular subscript-assignment / operator-overload path runs cleanly. Once the base
// is known to be `Extern<T>` we commit, and the indices and right-hand side are synthesized with
// diagnostics enabled.
// Chains such as `a.b.e[idx] = v`, `e.f1[idx] = v`, `a()[idx] = v` and `b["k"][idx] = v` are handled
// by the base: synthesizing it desugars any nested extern member/index/call access first, and
// `CloneEffectiveExpr` then picks up that desugared form as the receiver of `indexedUpdate`
// (e.g. `e.f1[idx] = v` becomes `T.indexedUpdate(T.memberAccess(e, "f1"), idx, v)`).
// Multiple indices `e[i1, ..., iN] = v` desugar into
// `T.indexedUpdate(T.indexedAccess(...T.indexedAccess(e, i1)..., i(N-1)), iN, v)`, matching the read-side
// chaining in `TypeCheckExternIndexAccess`.
bool TypeChecker::TypeCheckerImpl::TypeCheckExternIndexUpdate(ASTContext& ctx, AssignExpr& ae)
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
    Ptr<Ty> sourceExternTy = ProbeExternBaseTy(ctx, *se->baseExpr);
    if (!sourceExternTy) {
        return false;
    }

    // Committed to the extern desugaring. Synthesize the indices and the right-hand side value (so
    // that nested extern expressions get type-checked) and pin their ideal types; otherwise
    // post-typecheck AST validation rejects the ideal-typed nodes.
    for (auto& indexExpr : se->indexExprs) {
        if (!indexExpr) {
            ae.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
        Synthesize({ctx, SynPos::EXPR_ARG}, indexExpr);
        ReplaceIdealTy(*indexExpr);
    }
    Synthesize({ctx, SynPos::EXPR_ARG}, ae.rightExpr);
    ReplaceIdealTy(*ae.rightExpr);
    ReplaceIdealTy(*se->baseExpr);

    bool valid = Ty::IsTyCorrect(ae.rightExpr->GetTy()) &&
        std::all_of(se->indexExprs.cbegin(), se->indexExprs.cend(),
            [](auto& indexExpr) { return indexExpr && Ty::IsTyCorrect(indexExpr->GetTy()); });
    if (!valid) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }

    // Dynamic extern index update. Assign the `Unit` type now and tag the assignment; the
    // `T.indexedUpdate(...)` chain is built later by `DesugarExternIndexUpdate`.
    ae.SetTy(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));
    ae.EnableAttr(Attribute::EXTERN_PENDING_DESUGAR);
    return true;
}

void TypeChecker::TypeCheckerImpl::DesugarExternIndexUpdate(AssignExpr& ae)
{
    auto se = DynamicCast<SubscriptExpr*>(ae.leftValue.get());
    CJC_NULLPTR_CHECK(se);
    CJC_NULLPTR_CHECK(se->baseExpr);
    auto sourceExternTy = se->baseExpr->GetTy();
    CJC_ASSERT(Ty::IsTyCorrect(sourceExternTy) && TypeIsExtern(*sourceExternTy));

    auto info = ResolveExternRuntime(*sourceExternTy);

    // Receiver of the final `indexedUpdate`: the base, with all but the last index applied via
    // `T.indexedAccess(base, idx)` (the read part of the chain).
    OwnedPtr<Expr> receiver = CloneEffectiveExpr(*se->baseExpr);
    for (size_t i = 0; i + 1 < se->indexExprs.size(); ++i) {
        receiver = CreateRuntimeIndexAccess(*se, info, std::move(receiver), *se->indexExprs[i], *sourceExternTy);
    }

    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);

    Ptr<FuncDecl> indexedUpdateDecl = nullptr;
    auto indexedUpdate = CreateRuntimeMemberAccess(ae, info, EXTERN_INDEXED_UPDATE, indexedUpdateDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(receiver)));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*se->indexExprs.back())));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(*ae.rightExpr)));

    auto call = CreateRuntimeCall(ae, info, std::move(indexedUpdate), *indexedUpdateDecl, std::move(args), *unitTy);

    call->SetTy(unitTy);
    ae.desugarExpr = std::move(call);
}

// `e(args...)` -> `T.functionCall(e, argsArray)` for `e: Extern<T>`.
// `e.foo(args...)` -> `T.functionCall(T.memberAccess(e, "foo"), argsArray)` for `e: Extern<T>`.
// More generally, the callee `ce.baseFunc` is treated as a value: if its type is `Extern<T>`,
// then the whole call is desugared into `T.functionCall(callee, [args...])`. Any dynamic member
// or index accesses contained in the callee are desugared by their own rules when the callee is
// synthesized, so e.g. `q.w.e.f1.f2(args)` becomes
// `T.functionCall(T.memberAccess(T.memberAccess(q.w.e, "f1"), "f2"), [args...])`.
bool TypeChecker::TypeCheckerImpl::TypeCheckExternFunctionCall(ASTContext& ctx, CallExpr& ce)
{
    if (!ce.baseFunc) {
        return false;
    }
    Ptr<Expr> base = ce.baseFunc.get();

    // Determine the type of the callee interpreted as a value (the callee has not been synthesized
    // yet at this point). If it is not `Extern<T>`, this is an ordinary call that must be left to the
    // normal call-checking path.
    Ptr<Ty> baseTy = ProbeExternBaseTy(ctx, *base);
    if (!baseTy) {
        return false;
    }

    // A reference to a type (e.g. the constructor call `Extern<T>(payload)`) is not a value call and
    // must be handled by the regular call-checking path.
    if (auto tgt = base->GetTarget(); tgt && tgt->IsTypeDecl()) {
        ctx.ClearTypeCheckCache(*base);
        return false;
    }

    // Synthesize the user-provided arguments (so that they are type-checked) and pin their ideal
    // types before they are later cloned as the elements of the `Array<Any>` passed to `functionCall`.
    for (auto& arg : ce.args) {
        if (!arg || !arg->expr) {
            ce.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
        Synthesize({ctx, SynPos::EXPR_ARG}, arg->expr);
        ReplaceIdealTy(*arg->expr);
        if (!Ty::IsTyCorrect(arg->expr->GetTy())) {
            ce.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
    }

    // Dynamic extern value call. Assign the `Extern<T>` type now and tag the call; the
    // `T.functionCall(callee, [args...])` lowering is deferred to `DesugarExternFunctionCall`.
    ce.SetTy(baseTy);
    ce.EnableAttr(Attribute::EXTERN_PENDING_DESUGAR);
    return true;
}

void TypeChecker::TypeCheckerImpl::DesugarExternFunctionCall(CallExpr& ce)
{
    CJC_NULLPTR_CHECK(ce.baseFunc);
    Ptr<Expr> base = ce.baseFunc.get();
    auto sourceExternTy = base->GetTy();
    CJC_ASSERT(Ty::IsTyCorrect(sourceExternTy) && TypeIsExtern(*sourceExternTy));
    auto info = ResolveExternRuntime(*sourceExternTy);

    // Collect the (already type-checked) arguments as the elements of the `Array<Any>`.
    std::vector<OwnedPtr<Expr>> elements;
    for (auto& arg : ce.args) {
        CJC_NULLPTR_CHECK(arg);
        CJC_NULLPTR_CHECK(arg->expr);
        elements.emplace_back(CloneEffectiveExpr(*arg->expr));
    }

    // Build the `Array<Any>` literal holding the arguments.
    auto arrayDecl = importManager.GetCoreDecl<StructDecl>(STD_LIB_ARRAY);
    CJC_ASSERT(arrayDecl);
    auto arrayTy = typeManager.GetStructTy(*arrayDecl, {typeManager.GetAnyTy()});
    auto arrayLit = CreateArrayLit(std::move(elements), arrayTy);
    AddArrayLitConstructor(*arrayLit);
    arrayLit->EnableAttr(Attribute::COMPILER_ADD, Attribute::EXTERN_DESUGAR);
    arrayLit->curFile = ce.curFile;
    CopyBasicInfo(&ce, arrayLit.get());

    // Build `T.functionCall(callee, argsArray)`.
    Ptr<FuncDecl> functionCallDecl = nullptr;
    auto functionCall = CreateRuntimeMemberAccess(ce, info, EXTERN_FUNCTION_CALL, functionCallDecl);

    OwnedPtr<Expr> calleeExpr = CloneEffectiveExpr(*base);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(calleeExpr), sourceExternTy));
    args.emplace_back(CreateExternDesugarArg(std::move(arrayLit), arrayTy));

    auto call =
        CreateRuntimeCall(ce, info, std::move(functionCall), *functionCallDecl, std::move(args), *sourceExternTy);

    call->SetTy(sourceExternTy);
    ce.desugarExpr = std::move(call);
}

bool TypeChecker::TypeCheckerImpl::TypeIsExtern(Ty& ty)
{
    // Extern declaration always exists
    auto externDecl = importManager.GetCoreDecl<StructDecl>(EXTERN_TYPE_EXTERN);
    CJC_ASSERT(externDecl);

    // Check if the type of the actual target is valid, and is Extern. If not, externification is unnecessary
    auto structTy = DynamicCast<StructTy*>(&ty);
    return structTy && structTy->declPtr == externDecl && structTy->typeArgs.size() == 1;
}

Ptr<FuncDecl> TypeChecker::TypeCheckerImpl::GetRuntimeFuncDecl(const std::string& name)
{
    auto runtimeDecl = importManager.GetCoreDecl<InterfaceDecl>(EXTERN_TYPE_RUNTIME);
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
        auto runtimeInterfaceDecl = importManager.GetCoreDecl<InterfaceDecl>(EXTERN_TYPE_RUNTIME);
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
    // Sanity check shared by all runtime desugarings: a non-generic runtime call must resolve to the
    // runtime function it was built from and produce (a subtype of) the requested return type.
    CJC_ASSERT(info.isGeneric ||
        (call->resolvedFunction && call->resolvedFunction->identifier == decl.identifier &&
            typeManager.IsSubtype(call->GetTy(), &retTy)));
    return call;
}

OwnedPtr<CallExpr> TypeChecker::TypeCheckerImpl::CreateRuntimeIndexAccess(
    const Expr& srcNode, const ExternRuntimeInfo& info, OwnedPtr<Expr> baseExpr, Expr& indexExpr, Ty& ty)
{
    Ptr<FuncDecl> indexAccessDecl = nullptr;
    auto indexedAccess = CreateRuntimeMemberAccess(srcNode, info, EXTERN_INDEXED_ACCESS, indexAccessDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(baseExpr)));
    args.emplace_back(CreateExternDesugarArg(CloneEffectiveExpr(indexExpr)));

    auto call = CreateRuntimeCall(srcNode, info, std::move(indexedAccess), *indexAccessDecl, std::move(args), ty);
    call->SetTy(&ty);
    return call;
}

// Desugar a forced cast to `R.fromExtern<U>(operand)`. The operand is moved in (no
// clone) so nested casts stay linear.
OwnedPtr<CallExpr> TypeChecker::TypeCheckerImpl::BuildForcedCastCall(
    AmbiguousForcedCastExpr& afce, Ptr<Ty> targetTy, Ptr<Ty> operandTy)
{
    auto info = ResolveExternRuntime(*operandTy);
    Ptr<FuncDecl> decl = nullptr;
    auto member = CreateRuntimeMemberAccess(afce, info, EXTERN_FROM_EXTERN, decl);
    if (!decl) {
        return nullptr;
    }
    // fromExtern<U>: the explicit type argument and callee type `(Extern<R>) -> U`.
    member->instTys.clear();
    member->instTys.emplace_back(targetTy);
    member->typeArguments.clear();
    member->typeArguments.emplace_back(ASTCloner::Clone(afce.type.get()));
    member->SetTy(typeManager.GetFunctionTy({operandTy}, targetTy));

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateExternDesugarArg(std::move(afce.rightExpr), operandTy));
    auto call = CreateRuntimeCall(afce, info, std::move(member), *decl, std::move(args), *targetTy);
    call->SetTy(targetTy);
    return call;
}

void TypeChecker::TypeCheckerImpl::DesugarExternForcedCast(AmbiguousForcedCastExpr& afce)
{
    CJC_NULLPTR_CHECK(afce.type);
    CJC_NULLPTR_CHECK(afce.rightExpr);
    auto targetTy = afce.type->GetTy();
    auto operandTy = afce.rightExpr->GetTy();
    CJC_ASSERT(Ty::IsTyCorrect(targetTy) && Ty::IsTyCorrect(operandTy) && TypeIsExtern(*operandTy));
    auto call = BuildForcedCastCall(afce, targetTy, operandTy);
    CJC_ASSERT(call);
    afce.desugarExpr = std::move(call);
}

// Strip cached types/desugars from a cloned subtree so the enclosing call re-checks
// it cleanly (ASTCloner copies types).
static void StripTypesForRecheck(Ptr<Node> root)
{
    Walker(root, [](Ptr<Node> n) -> VisitAction {
        // Reset to the initial (unchecked) type rather than null: SetTy asserts non-null
        // in debug builds, and the initial ty is what an un-type-checked node carries.
        n->SetTy(Ty::GetInitialTy());
        if (auto ex = DynamicCast<Expr*>(n.get())) {
            ex->desugarExpr = nullptr;
        }
        return VisitAction::WALK_CHILDREN;
    }).Walk();
}

// Ordinary reading `U(args)` of a `(U)(args)` whose forced cast did not apply (call
// form only). When `U` is a type the operand was already synthesised as a cast probe,
// so its args are taken as freshly re-typed clones; otherwise the pristine operand is
// moved in (also the deeply nested path, where cloning would be exponential).
Ptr<Ty> TypeChecker::TypeCheckerImpl::DesugarAmbiguousOrdinaryCall(
    const CheckerContext& ctx, AmbiguousForcedCastExpr& afce, bool typeValid)
{
    bool freshen = typeValid;
    auto take = [freshen](OwnedPtr<Expr>& e) -> OwnedPtr<Expr> {
        if (!freshen) {
            return std::move(e);
        }
        auto cloned = ASTCloner::Clone(e.get());
        StripTypesForRecheck(cloned.get());
        return cloned;
    };
    std::vector<OwnedPtr<FuncArg>> args;
    auto& operand = afce.rightExpr;
    if (operand->astKind == ASTKind::TUPLE_LIT) {
        for (auto& child : StaticAs<ASTKind::TUPLE_LIT>(operand.get())->children) {
            args.emplace_back(CreateFuncArg(take(child)));
        }
    } else if (operand->astKind == ASTKind::PAREN_EXPR) {
        args.emplace_back(CreateFuncArg(take(StaticAs<ASTKind::PAREN_EXPR>(operand.get())->expr)));
    } else if (!(operand->astKind == ASTKind::LIT_CONST_EXPR &&
                   StaticAs<ASTKind::LIT_CONST_EXPR>(operand.get())->kind == LitConstKind::UNIT)) {
        args.emplace_back(CreateFuncArg(take(operand)));
    }
    auto call = CreateCallExpr(std::move(afce.leftExpr), std::move(args));
    CopyBasicInfo(&afce, call.get());
    call->sourceExpr = &afce;
    afce.desugarExpr = std::move(call);
    auto ty = SynthesizeWithoutRecover(ctx, afce.desugarExpr.get());
    if (Ty::IsTyCorrect(ty)) {
        afce.SetTy(ty);
        return ty;
    }
    // `U(args)` did not type-check. If `U` is a type this was a malformed forced cast
    // (a type-name call like `Foo(1)` fails silently here), so drop the dead call and
    // report cleanly; otherwise the call already emitted its own error.
    if (typeValid) {
        afce.desugarExpr = nullptr;
        diag.DiagnoseRefactor(DiagKindRefactor::sema_invalid_forced_cast_expr, afce);
    }
    afce.SetTy(TypeManager::GetInvalidTy());
    return TypeManager::GetInvalidTy();
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynAmbiguousForcedCastExpr(
    const CheckerContext& ctx, AmbiguousForcedCastExpr& afce)
{
    // An extern forced cast that was already type-checked (its lowering is deferred to
    // DESUGAR_AFTER_SEMA) carries a valid type but no desugar yet; return the resolved type.
    if (afce.TestAttr(Attribute::EXTERN_PENDING_DESUGAR) && Ty::IsTyCorrect(afce.GetTy())) {
        return afce.GetTy();
    }
    if (afce.desugarExpr) {
        if (Ty::IsTyCorrect(afce.GetTy())) {
            return afce.GetTy();
        }
        // A cloned AFC (e.g. from function-argument overload resolution) can carry a
        // copied desugar whose type was left invalid because the source was still
        // unresolved when cloned. Re-type the existing desugar instead of returning
        // the stale invalid type; `rightExpr` may already have been consumed when the
        // original was desugared, so it cannot be re-resolved from scratch here.
        auto ty = Synthesize(ctx, afce.desugarExpr.get());
        afce.SetTy(ty);
        return ty;
    }
    CJC_NULLPTR_CHECK(afce.rightExpr);

    // Resolve `U` under diagnostic suppression: it may legitimately not be a type
    // (e.g. `(consume)(e)`), in which case the ordinary reading applies and the
    // "not a type" error must not surface.
    Ptr<Ty> targetTy = TypeManager::GetInvalidTy();
    if (afce.type) {
        auto ds = DiagSuppressor(diag);
        SetTypeTy(ctx.Ctx(), *afce.type);
        targetTy = afce.type->GetTy();
        if (Ty::IsTyCorrect(targetTy)) {
            ds.ReportDiag();
        }
    }
    bool typeValid = afce.type && Ty::IsTyCorrect(targetTy);
    // Call form `(U)(args)` (its operand is the `(args)` parse: ParenExpr / TupleLit
    // / unit) is the only one with an ordinary reading `U(args)`; juxtaposition
    // `(U)e` never yields those at top level, so the kind alone discriminates them.
    auto& rhs = afce.rightExpr;
    bool isCall = rhs && (rhs->astKind == ASTKind::PAREN_EXPR || rhs->astKind == ASTKind::TUPLE_LIT ||
        (rhs->astKind == ASTKind::LIT_CONST_EXPR &&
            StaticAs<ASTKind::LIT_CONST_EXPR>(rhs.get())->kind == LitConstKind::UNIT));
    if (typeValid) {
        auto operandTy = Synthesize(ctx, afce.rightExpr.get());
        // A forced cast `(U)e` of an `Extern<R>` operand is a `fromExtern<U>` conversion. Assign the
        // target type now and tag the node; the `R.fromExtern<U>(e)` lowering is deferred to
        // `DesugarExternForcedCast`. The deferral requires a real `fromExtern` runtime member;
        // otherwise fall through to the ordinary reading.
        if (Ty::IsTyCorrect(operandTy) && TypeIsExtern(*operandTy) && GetRuntimeFuncDecl(EXTERN_FROM_EXTERN)) {
            afce.SetTy(targetTy);
            // The forced-cast reading only needs `type` and `rightExpr`; the alternative "ordinary call"
            // interpretation `leftExpr` (`U` parsed as an expression) is now dead. Because the lowering is
            // deferred, the un-desugared node survives to the post-typecheck AST validator, which would
            // otherwise walk this dead `leftExpr` and reject its unresolved type. Drop it now (the old
            // inline lowering avoided this by filling `desugarExpr` before validation).
            afce.leftExpr = nullptr;
            afce.EnableAttr(Attribute::EXTERN_PENDING_DESUGAR);
            return targetTy;
        }
    }

    // Ordinary reading `U(args)` (call form only). When `U` is a type the operand
    // was already synthesised above (cast probe), so its args are taken as freshly
    // re-typed clones, else overload resolution silently keeps a pre-checked arg.
    // When `U` is not a type the operand is pristine and moved in — also the deeply
    // nested path, where cloning would be exponential (a real type only appears at
    // shallow, bounded cast sites).
    if (isCall && afce.leftExpr && afce.rightExpr) {
        return DesugarAmbiguousOrdinaryCall(ctx, afce, typeValid);
    }

    // Juxtaposition `(U)e` with no ordinary reading: a malformed forced cast when `U`
    // is a type, otherwise let the operand's own diagnostics surface.
    if (typeValid) {
        diag.DiagnoseRefactor(DiagKindRefactor::sema_invalid_forced_cast_expr, afce);
    } else {
        (void)Synthesize(ctx, afce.rightExpr.get());
    }
    afce.SetTy(TypeManager::GetInvalidTy());
    return TypeManager::GetInvalidTy();
}

// Lower every extern operation that was type-checked and tagged during SEMA into its `Runtime` call.
// This mirrors the other after-typecheck desugarings (`is`/`as`/`range`/...): it walks the live,
// fully type-checked package AST and builds each `Runtime` call straight from the tagged node's own
// operands. The walk is post-order, so a node is lowered only after the nested extern sub-expressions
// it contains; those nested nodes are then picked up through their freshly built `desugarExpr` by
// `CloneEffectiveExpr`.
//
// A structural operation and a coercion can both apply to the same node: an extern update
// (`e.foo = v` / `e[i] = v`) yields `Unit`, so when used where `Extern<T>` is expected (e.g. as the
// body of a function returning `Extern<T>`) it is *also* coerced. Such a node carries both tags; the
// structural lowering runs first and the coercion then wraps its `desugarExpr` in `T.toExtern`.
void TypeChecker::TypeCheckerImpl::DesugarExternInPackage(Package& pkg)
{
    bool desugared = false;
    auto postVisit = [this, &desugared](Ptr<Node> node) -> VisitAction {
        auto expr = DynamicCast<Expr*>(node.get());
        if (!expr || expr->desugarExpr) {
            return VisitAction::WALK_CHILDREN;
        }
        // Structural extern operation first: build its `Runtime` call into `desugarExpr`.
        if (expr->TestAttr(Attribute::EXTERN_PENDING_DESUGAR)) {
            switch (node->astKind) {
                case ASTKind::MEMBER_ACCESS:
                    DesugarExternMemberAccess(*StaticCast<MemberAccess*>(node.get()));
                    break;
                case ASTKind::SUBSCRIPT_EXPR:
                    DesugarExternIndexAccess(*StaticCast<SubscriptExpr*>(node.get()));
                    break;
                case ASTKind::CALL_EXPR:
                    DesugarExternFunctionCall(*StaticCast<CallExpr*>(node.get()));
                    break;
                case ASTKind::ASSIGN_EXPR: {
                    auto ae = StaticCast<AssignExpr*>(node.get());
                    if (ae->leftValue && ae->leftValue->astKind == ASTKind::SUBSCRIPT_EXPR) {
                        DesugarExternIndexUpdate(*ae);
                    } else {
                        DesugarExternMemberUpdate(*ae);
                    }
                    break;
                }
                case ASTKind::AMBIGUOUS_FORCED_CAST_EXPR:
                    DesugarExternForcedCast(*StaticCast<AmbiguousForcedCastExpr*>(node.get()));
                    break;
                default:
                    break;
            }
            expr->DisableAttr(Attribute::EXTERN_PENDING_DESUGAR);
            desugared = true;
        }
        // Coercion, if any, wraps the (now possibly desugared) sub-expression in `T.toExtern`. It needs
        // the source type `R` recorded by `CoerceToExtern` (the node's own type has since been overwritten
        // to `Extern<T>`). Because the walk is post-order, the coerced sub-expression has already had any
        // nested extern operations lowered, so `CloneEffectiveExpr` picks up their `desugarExpr`.
        if (expr->TestAttr(Attribute::EXTERN_PENDING_COERCE)) {
            if (auto it = externCoerceSourceTy.find(expr); it != externCoerceSourceTy.end()) {
                DesugarExternCoerce(*expr, it->second);
                expr->DisableAttr(Attribute::EXTERN_PENDING_COERCE);
                desugared = true;
            }
        }
        return VisitAction::WALK_CHILDREN;
    };
    Walker(&pkg, nullptr, postVisit).Walk();
    externCoerceSourceTy.clear();

    // The operand subexpressions cloned into the freshly built `Runtime` calls (base, indices,
    // right-hand side, call arguments) were type-checked during SEMA, before property `get`/`set`
    // accesses were lowered by `DesugarForPropDecl` in `PostTypeCheck`. That pass has already run and
    // will not revisit these new subtrees, so a property access inside an extern operand (e.g.
    // `e.f = arr.size`) would otherwise reach CHIR as a raw property member access. Re-run it over the
    // package to lower the property accesses now living inside the extern `desugarExpr`s (already-lowered
    // nodes are skipped, so this is idempotent for the rest of the tree). Other post-typecheck
    // desugarings run after this point and walk into `desugarExpr`, so they cover the clones without
    // special handling.
    if (desugared) {
        DesugarForPropDecl(pkg);
    }
}

bool TypeChecker::TypeCheckerImpl::ChkAmbiguousForcedCastExpr(
    ASTContext& ctx, Ty& target, AmbiguousForcedCastExpr& afce)
{
    CheckerContext cctx{ctx, SynPos::EXPR_ARG};
    auto ty = SynAmbiguousForcedCastExpr(cctx, afce);
    if (Ty::IsTyCorrect(ty) && typeManager.IsSubtype(ty, &target)) {
        return true;
    }
    if (Ty::IsTyCorrect(ty)) {
        DiagMismatchedTypes(diag, afce, target);
    }
    afce.SetTy(TypeManager::GetInvalidTy());
    return false;
}

} // namespace Cangjie
