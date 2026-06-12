// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "BuiltInOperatorUtil.h"
#include "Desugar/DesugarInTypeCheck.h"
#include "DiagSuppressor.h"
#include "Diags.h"
#include "TypeCheckUtil.h"

#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/RecoverDesugar.h"

using namespace Cangjie;
using namespace Sema;
using namespace TypeCheckUtil;

namespace {
const std::vector<TokenKind> SHIFT_ASSIGN_OPERATOR = {TokenKind::LSHIFT_ASSIGN, TokenKind::RSHIFT_ASSIGN};

OwnedPtr<Expr> CloneEffectiveExpr(Ptr<Expr> expr)
{
    if (expr && expr->desugarExpr) {
        return ASTCloner::Clone(Ptr(expr->desugarExpr.get()));
    }
    return ASTCloner::Clone(expr);
}

template <typename F>
bool TryDesugarExternMemberUpdate(
    TypeManager& typeManager, ImportManager& importManager, AssignExpr& ae, F typecheck)
{
    if (ae.isCompound) {
        return false;
    }
    auto ma = DynamicCast<MemberAccess*>(ae.leftValue.get());
    if (!ma || !ma->baseExpr || !Ty::IsTyCorrect(ma->baseExpr->GetTy()) ||
        !TypeIsExtern(importManager, ma->baseExpr->GetTy())) {
        return false;
    }

    Ptr<Ty> rightTy = typecheck(ae.rightExpr.get());
    if (!Ty::IsTyCorrect(rightTy)) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }
    rightTy = ae.rightExpr->GetTy();

    auto sourceExternTy = ma->baseExpr->GetTy();
    auto runtimeTy = sourceExternTy->typeArgs[0];
    CJC_ASSERT(Ty::IsTyCorrect(runtimeTy));

    Ptr<Decl> runtimeDecl = nullptr;
    bool isGenericRuntimeTy = false;
    if (auto genericsTy = DynamicCast<GenericsTy*>(runtimeTy); genericsTy) {
        isGenericRuntimeTy = true;
        runtimeDecl = genericsTy->decl;
    } else {
        runtimeDecl = Ty::GetDeclPtrOfTy<Decl>(runtimeTy);
    }
    CJC_ASSERT(runtimeDecl);

    auto runtimeRef = CreateRefExpr(*runtimeDecl);
    runtimeRef->isAlone = false;
    runtimeRef->SetTy(runtimeTy);
    runtimeRef->EnableAttr(Attribute::COMPILER_ADD);
    CopyBasicInfo(&ae, runtimeRef.get());

    OwnedPtr<MemberAccess> memberUpdate;
    Ptr<FuncDecl> memberUpdateDecl = nullptr;
    if (isGenericRuntimeTy) {
        memberUpdate = MakeOwned<MemberAccess>();
        memberUpdate->baseExpr = std::move(runtimeRef);
        memberUpdate->field = "memberUpdate";
        memberUpdateDecl = GetRuntimeFuncDecl(importManager, "memberUpdate");
    } else {
        memberUpdate = CreateMemberAccess(std::move(runtimeRef), "memberUpdate");
        memberUpdateDecl = DynamicCast<FuncDecl*>(memberUpdate->target);
    }
    memberUpdate->isAlone = false;
    memberUpdate->EnableAttr(Attribute::COMPILER_ADD);
    CopyBasicInfo(&ae, memberUpdate.get());
    if (!memberUpdateDecl) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }
    if (!isGenericRuntimeTy) {
        memberUpdate->targets.emplace_back(memberUpdateDecl);
    }

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateFuncArg(CloneEffectiveExpr(Ptr(ma->baseExpr.get()))));
    args.emplace_back(CreateFuncArg(CreateStringLit(importManager, ma->field.Val())));
    args.emplace_back(CreateFuncArg(CloneEffectiveExpr(Ptr(ae.rightExpr.get()))));

    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    auto callTarget = isGenericRuntimeTy ? Ptr<FuncDecl>() : memberUpdateDecl;
    auto call =
        CreateCallExpr(std::move(memberUpdate), std::move(args), callTarget, unitTy, CallKind::CALL_DECLARED_FUNCTION);
    CopyBasicInfo(&ae, call.get());
    call->sourceExpr = &ae;
    call->EnableAttr(Attribute::COMPILER_ADD);

    Ptr<Ty> callTy = typecheck(call.get());
    bool hasExpectedTarget = isGenericRuntimeTy ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "memberUpdate");
    bool isOk = Ty::IsTyCorrect(callTy) && hasExpectedTarget && typeManager.IsSubtype(call->GetTy(), unitTy);

    Ptr<Ty> resultTy = isOk ? Ptr<Ty>(unitTy) : Ptr<Ty>(TypeManager::GetInvalidTy());
    ae.SetTy(resultTy);
    call->SetTy(resultTy);
    if (isOk) {
        ae.desugarExpr = std::move(call);
    }
    return true;
}

template <typename F>
bool TryDesugarExternIndexUpdate(TypeManager& typeManager, ImportManager& importManager, AssignExpr& ae, F typecheck)
{
    if (ae.isCompound) {
        return false;
    }
    auto se = DynamicCast<SubscriptExpr*>(ae.leftValue.get());
    if (!se || !se->baseExpr || se->indexExprs.empty()) {
        return false;
    }

    SetIsNotAlone(*se->baseExpr);
    Ptr<Ty> sourceExternTy = typecheck(se->baseExpr.get());
    for (auto& indexExpr : se->indexExprs) {
        (void)typecheck(indexExpr.get());
    }
    if (!Ty::IsTyCorrect(sourceExternTy) || !TypeIsExtern(importManager, sourceExternTy)) {
        return false;
    }
    if (!std::all_of(se->indexExprs.cbegin(), se->indexExprs.cend(), [](auto& indexExpr) {
        return indexExpr && Ty::IsTyCorrect(indexExpr->GetTy());
    })) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }

    Ptr<Ty> rightTy = typecheck(ae.rightExpr.get());
    if (!Ty::IsTyCorrect(rightTy)) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }
    rightTy = ae.rightExpr->GetTy();

    auto runtimeTy = sourceExternTy->typeArgs[0];
    CJC_ASSERT(Ty::IsTyCorrect(runtimeTy));
    Ptr<Decl> runtimeDecl = nullptr;
    bool isGenericRuntimeTy = false;
    if (auto genericsTy = DynamicCast<GenericsTy*>(runtimeTy); genericsTy) {
        isGenericRuntimeTy = true;
        runtimeDecl = genericsTy->decl;
    } else {
        runtimeDecl = Ty::GetDeclPtrOfTy<Decl>(runtimeTy);
    }
    CJC_ASSERT(runtimeDecl);

    auto createRuntimeAccess = [&](OwnedPtr<Expr> baseExpr, Expr& indexExpr) -> OwnedPtr<CallExpr> {
        auto runtimeRef = CreateRefExpr(*runtimeDecl);
        runtimeRef->isAlone = false;
        runtimeRef->SetTy(runtimeTy);
        runtimeRef->EnableAttr(Attribute::COMPILER_ADD);
        CopyBasicInfo(&ae, runtimeRef.get());

        OwnedPtr<MemberAccess> indexAccess;
        Ptr<FuncDecl> indexAccessDecl = nullptr;
        if (isGenericRuntimeTy) {
            indexAccess = MakeOwned<MemberAccess>();
            indexAccess->baseExpr = std::move(runtimeRef);
            indexAccess->field = "indexAccess";
            indexAccessDecl = GetRuntimeFuncDecl(importManager, "indexAccess");
            indexAccess->target = indexAccessDecl;
            auto runtimeInterfaceDecl = importManager.GetCoreDecl<InterfaceDecl>("Runtime");
            CJC_ASSERT(runtimeInterfaceDecl);
            if (indexAccessDecl) {
                auto typeMapping = GenerateTypeMapping(*runtimeInterfaceDecl, {runtimeTy});
                indexAccess->SetTy(typeManager.GetInstantiatedTy(indexAccessDecl->GetTy(), typeMapping));
            }
        } else {
            indexAccess = CreateMemberAccess(std::move(runtimeRef), "indexAccess");
            indexAccessDecl = DynamicCast<FuncDecl*>(indexAccess->target);
        }
        indexAccess->isAlone = false;
        indexAccess->EnableAttr(Attribute::COMPILER_ADD);
        CopyBasicInfo(&ae, indexAccess.get());
        if (!indexAccessDecl) {
            return nullptr;
        }
        indexAccess->targets.emplace_back(indexAccessDecl);

        std::vector<OwnedPtr<FuncArg>> args;
        args.emplace_back(CreateFuncArg(std::move(baseExpr)));
        args.emplace_back(CreateFuncArg(CloneEffectiveExpr(Ptr(&indexExpr))));

        auto callTarget = isGenericRuntimeTy ? Ptr<FuncDecl>() : indexAccessDecl;
        auto call = CreateCallExpr(
            std::move(indexAccess), std::move(args), callTarget, sourceExternTy, CallKind::CALL_DECLARED_FUNCTION);
        CopyBasicInfo(&ae, call.get());
        call->sourceExpr = &ae;
        call->EnableAttr(Attribute::COMPILER_ADD);
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

    auto runtimeRef = CreateRefExpr(*runtimeDecl);
    runtimeRef->isAlone = false;
    runtimeRef->SetTy(runtimeTy);
    runtimeRef->EnableAttr(Attribute::COMPILER_ADD);
    CopyBasicInfo(&ae, runtimeRef.get());

    OwnedPtr<MemberAccess> indexUpdate;
    Ptr<FuncDecl> indexUpdateDecl = nullptr;
    if (isGenericRuntimeTy) {
        indexUpdate = MakeOwned<MemberAccess>();
        indexUpdate->baseExpr = std::move(runtimeRef);
        indexUpdate->field = "indexUpdate";
        indexUpdateDecl = GetRuntimeFuncDecl(importManager, "indexUpdate");
        indexUpdate->target = indexUpdateDecl;
        auto runtimeInterfaceDecl = importManager.GetCoreDecl<InterfaceDecl>("Runtime");
        CJC_ASSERT(runtimeInterfaceDecl);
        if (indexUpdateDecl) {
            auto typeMapping = GenerateTypeMapping(*runtimeInterfaceDecl, {runtimeTy});
            indexUpdate->SetTy(typeManager.GetInstantiatedTy(indexUpdateDecl->GetTy(), typeMapping));
        }
    } else {
        indexUpdate = CreateMemberAccess(std::move(runtimeRef), "indexUpdate");
        indexUpdateDecl = DynamicCast<FuncDecl*>(indexUpdate->target);
    }
    indexUpdate->isAlone = false;
    indexUpdate->EnableAttr(Attribute::COMPILER_ADD);
    CopyBasicInfo(&ae, indexUpdate.get());
    if (!indexUpdateDecl) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return true;
    }
    indexUpdate->targets.emplace_back(indexUpdateDecl);

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateFuncArg(std::move(updateBaseExpr)));
    args.emplace_back(CreateFuncArg(CloneEffectiveExpr(Ptr(se->indexExprs.back().get()))));
    args.emplace_back(CreateFuncArg(CloneEffectiveExpr(Ptr(ae.rightExpr.get()))));

    auto unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    auto callTarget = isGenericRuntimeTy ? Ptr<FuncDecl>() : indexUpdateDecl;
    auto call =
        CreateCallExpr(std::move(indexUpdate), std::move(args), callTarget, unitTy, CallKind::CALL_DECLARED_FUNCTION);
    CopyBasicInfo(&ae, call.get());
    call->sourceExpr = &ae;
    call->EnableAttr(Attribute::COMPILER_ADD);

    Ptr<Ty> callTy = typecheck(call.get());
    bool hasExpectedTarget = isGenericRuntimeTy ||
        (call->resolvedFunction && call->resolvedFunction->identifier == "indexUpdate");
    bool isOk = Ty::IsTyCorrect(callTy) && hasExpectedTarget && typeManager.IsSubtype(call->GetTy(), unitTy);

    Ptr<Ty> resultTy = isOk ? Ptr<Ty>(unitTy) : Ptr<Ty>(TypeManager::GetInvalidTy());
    ae.SetTy(resultTy);
    call->SetTy(resultTy);
    if (isOk) {
        ae.desugarExpr = std::move(call);
    }
    return true;
}

/**
 * This table encodes the types of the operator and result, for every assignment operator, where key is assignment
 * operator kind, and value is supported type for corresponding kind of assignment.
 */
#define ASSIGNABLE_INTEGER_TYPE                                                                                        \
    TypeKind::TYPE_INT8, TypeKind::TYPE_INT16, TypeKind::TYPE_INT32, TypeKind::TYPE_INT64, TypeKind::TYPE_INT_NATIVE,  \
        TypeKind::TYPE_UINT8, TypeKind::TYPE_UINT16, TypeKind::TYPE_UINT32, TypeKind::TYPE_UINT64,                     \
        TypeKind::TYPE_UINT_NATIVE, TypeKind::TYPE_IDEAL_INT

#define ASSIGNABLE_NUMERIC_TYPE                                                                                        \
    ASSIGNABLE_INTEGER_TYPE, TypeKind::TYPE_FLOAT16, TypeKind::TYPE_FLOAT32, TypeKind::TYPE_FLOAT64,                   \
        TypeKind::TYPE_IDEAL_FLOAT

const std::map<TokenKind, std::vector<TypeKind>> COMPOUND_ASSIGN_TYPE_MAP = {
    {TokenKind::ADD_ASSIGN, {ASSIGNABLE_NUMERIC_TYPE}}, {TokenKind::SUB_ASSIGN, {ASSIGNABLE_NUMERIC_TYPE}},
    {TokenKind::MUL_ASSIGN, {ASSIGNABLE_NUMERIC_TYPE}}, {TokenKind::EXP_ASSIGN, {ASSIGNABLE_NUMERIC_TYPE}},
    {TokenKind::DIV_ASSIGN, {ASSIGNABLE_NUMERIC_TYPE}}, {TokenKind::MOD_ASSIGN, {ASSIGNABLE_INTEGER_TYPE}},
    {TokenKind::AND_ASSIGN, {TypeKind::TYPE_BOOLEAN}}, {TokenKind::OR_ASSIGN, {TypeKind::TYPE_BOOLEAN}},
    {TokenKind::BITAND_ASSIGN, {ASSIGNABLE_INTEGER_TYPE}}, {TokenKind::BITOR_ASSIGN, {ASSIGNABLE_INTEGER_TYPE}},
    {TokenKind::BITXOR_ASSIGN, {ASSIGNABLE_INTEGER_TYPE}}, {TokenKind::LSHIFT_ASSIGN, {ASSIGNABLE_INTEGER_TYPE}},
    {TokenKind::RSHIFT_ASSIGN, {ASSIGNABLE_INTEGER_TYPE}}};

void DiagCannotAssignToSubscript(DiagnosticEngine& diag, const SubscriptExpr& se, const Expr& rightValue)
{
    auto builder = diag.DiagnoseRefactor(DiagKindRefactor::sema_cannot_assign_to_subscript, se);
    CJC_ASSERT(se.baseExpr && Ty::IsTyCorrect(se.baseExpr->GetTy()));
    if (se.baseExpr->GetTy()->IsExtendable()) {
        std::string indexParams;
        for (size_t i = 0; i < se.indexExprs.size(); ++i) {
            CJC_ASSERT(se.indexExprs[i] && Ty::IsTyCorrect(se.indexExprs[i]->GetTy()));
            indexParams += "index" + std::to_string(i) + ": " + se.indexExprs[i]->GetTy()->String() + ", ";
        }
        builder.AddNote("you may want to implement 'operator func[](" + indexParams +
            "value!: " + rightValue.GetTy()->String() + ")' for type '" + se.baseExpr->GetTy()->String() + "'");
    }
}

bool MaySubscriptAssignOnlyBeOverload(const Expr& baseExpr)
{
    return Ty::IsTyCorrect(baseExpr.GetTy()) && !baseExpr.GetTy()->IsBuiltin() && !baseExpr.GetTy()->IsRange() &&
        !baseExpr.GetTy()->IsTuple();
}

// Check whether ScriptExpr can be modified and diagnose for immutable subscript expression.
bool IsAssignableSubScriptExpr(
    SubscriptExpr& se, bool isCompound, const std::vector<Diagnostic>& diags, DiagnosticEngine& diag)
{
    bool isOperatorOverload = se.desugarExpr != nullptr;
    if (se.desugarExpr) {
        RecoverToSubscriptExpr(se);
    }
    if (se.isTupleAccess) {
        (void)std::for_each(diags.begin(), diags.end(), [&diag](auto info) { (void)diag.Diagnose(info); });
        DiagInvalidAssign(diag, se, "tuple element");
        return false;
    }
    CJC_ASSERT(se.baseExpr && !se.indexExprs.empty());
    DiagKind diagKind =
        isCompound ? DiagKind::sema_subscript_get_set_not_supported : DiagKind::sema_subscript_set_not_supported;
    auto indexTy = TypeManager::GetNonNullTy(se.indexExprs[0]->GetTy());
    auto baseTy = TypeManager::GetNonNullTy(se.baseExpr->GetTy());
    if ((!baseTy->IsArray() && !Is<VArrayTy>(baseTy)) || (isOperatorOverload && !indexTy->IsInteger())) {
        (void)std::for_each(diags.begin(), diags.end(), [&diag](auto info) { (void)diag.Diagnose(info); });
        (void)diag.Diagnose(se, diagKind, baseTy->String(), indexTy->String());
        return false;
    }
    return true;
}

bool CheckMatchOfDimensionAndTypes(DiagnosticEngine& diag, TypeManager& typeManager,
    std::pair<const std::vector<Ptr<Ty>>&, uint64_t&> tysOfExprAndItsIndex,
    const std::pair<const TupleLit&, const TupleTy&>& tupleLitAndTy, const Expr& rightExpr)
{
    auto [tupleLit, tupleTy] = tupleLitAndTy;
    auto& [tysOfExpr, indexOfExprTys] = tysOfExprAndItsIndex;
    if (tupleLit.children.size() != tupleTy.typeArgs.size()) {
        DiagInvalidMultipleAssignExpr(diag, tupleLit, rightExpr,
            "the tuple has " + std::to_string(tupleLit.children.size()) + "-elements, found one with " +
                std::to_string(tupleTy.typeArgs.size()) + "-elements");
        return false;
    }
    uint64_t indexOfRightTy = 0;
    for (auto& leftValue : tupleLit.children) {
        CJC_NULLPTR_CHECK(leftValue);
        auto rightTy = tupleTy.typeArgs[indexOfRightTy];
        CJC_NULLPTR_CHECK(rightTy);
        indexOfRightTy++;
        const std::string leftDiagInfo = Ty::IsTyCorrect(leftValue->GetTy()) && !leftValue->IsInvalid()
            ? ("'" + leftValue->GetTy()->String() + "'")
            : "it";
        if (leftValue->astKind == ASTKind::TUPLE_LIT) {
            if (!rightTy->IsTuple()) {
                DiagInvalidMultipleAssignExpr(diag, *leftValue, rightExpr,
                    "can not assign " + leftDiagInfo + " with '" + rightExpr.GetTy()->String() + "'");
                return false;
            }
            auto tupleLitAndTupleTy = std::pair<const TupleLit&, const TupleTy&>(
                *StaticCast<TupleLit*>(leftValue.get()), *RawStaticCast<TupleTy*>(rightTy));
            bool success =
                CheckMatchOfDimensionAndTypes(diag, typeManager, tysOfExprAndItsIndex, tupleLitAndTupleTy, rightExpr);
            if (!success) {
                return false;
            }
            continue;
        }
        CJC_ASSERT(indexOfExprTys < tysOfExpr.size());
        bool typeMatch = (Ty::IsTyCorrect(leftValue->GetTy()) && typeManager.IsSubtype(rightTy, leftValue->GetTy())) ||
            Ty::IsTyCorrect(tysOfExpr[indexOfExprTys]);
        indexOfExprTys++;
        if (!typeMatch) {
            DiagInvalidMultipleAssignExpr(
                diag, *leftValue, rightExpr, "can not assign " + leftDiagInfo + " with '" + rightTy->String() + "'");
            return false;
        }
    }
    return true;
}

void CheckMultipleAssignExpr(DiagnosticEngine& diag, TypeManager& typeManager, AssignExpr& assignExpr)
{
    CJC_ASSERT(assignExpr.leftValue->astKind == ASTKind::TUPLE_LIT);
    auto& tupleLit = *StaticCast<TupleLit*>(assignExpr.leftValue.get());
    auto& rightExpr = *assignExpr.rightExpr;
    CJC_ASSERT(Ty::IsTyCorrect(rightExpr.GetTy()));
    // Collect tys of desugarExpr(except VarDecl) to help decide whether type match on corresponding single assign
    // expression.
    std::vector<Ptr<Ty>> tysOfDesugarExpr;
    // Index of tysOfDesugarExprs, increase when travel a non-tupleLit of leftValue.
    uint64_t indexOfDesugaredTys = 0;
    CJC_ASSERT(assignExpr.desugarExpr->astKind == ASTKind::BLOCK);
    for (auto& node : StaticCast<Block*>(assignExpr.desugarExpr.get())->body) {
        if (node->astKind == ASTKind::VAR_DECL) {
            continue;
        }
        (void)tysOfDesugarExpr.emplace_back(node->GetTy());
    }
    if (!rightExpr.GetTy()->IsTuple()) {
        const std::string leftDiagInfo = Ty::IsTyCorrect(tupleLit.GetTy()) && !tupleLit.IsInvalid()
            ? ("'" + tupleLit.GetTy()->String() + "'")
            : "it";
        DiagInvalidMultipleAssignExpr(diag, tupleLit, rightExpr,
            "can not assign " + leftDiagInfo + " with '" + rightExpr.GetTy()->String() + "'");
        assignExpr.SetTy(TypeManager::GetInvalidTy());
        return;
    }
    auto success = CheckMatchOfDimensionAndTypes(diag, typeManager, {tysOfDesugarExpr, indexOfDesugaredTys},
        {tupleLit, *RawStaticCast<TupleTy*>(rightExpr.GetTy())}, rightExpr);
    if (!success) {
        assignExpr.SetTy(TypeManager::GetInvalidTy());
    }
}
} // namespace

bool TypeChecker::TypeCheckerImpl::IsAssignable(Expr& e, bool isCompound, const std::vector<Diagnostic>& diags) const
{
    auto getTargetName = [](const Decl& decl) {
        return decl.TestAttr(AST::Attribute::MACRO_FUNC)
            ? "macro"
            : (decl.TestAttr(AST::Attribute::ENUM_CONSTRUCTOR) ? "enum constructor" : "func");
    };
    if (e.astKind == ASTKind::REF_EXPR) {
        auto& leftRef = static_cast<const RefExpr&>(e);
        if (leftRef.ref.target && leftRef.ref.target->astKind == ASTKind::FUNC_DECL) {
            (void)std::for_each(diags.begin(), diags.end(), [this](auto info) { (void)diag.Diagnose(info); });
            DiagInvalidAssign(diag, e, getTargetName(*leftRef.ref.target));
            return false;
        }
        if (leftRef.isThis && leftRef.isAlone) {
            (void)std::for_each(diags.begin(), diags.end(), [this](auto info) { (void)diag.Diagnose(info); });
            (void)diag.Diagnose(e, DiagKind::sema_invalid_assignment_to_this_expr);
            return false;
        }
    } else if (e.astKind == ASTKind::MEMBER_ACCESS) {
        auto& ma = static_cast<const MemberAccess&>(e);
        if (ma.target && ma.target->astKind == ASTKind::FUNC_DECL) {
            (void)std::for_each(diags.begin(), diags.end(), [this](auto info) { (void)diag.Diagnose(info); });
            DiagInvalidAssign(diag, e, getTargetName(*ma.target));
            return false;
        }
        bool isVArraySize = ma.baseExpr && Ty::IsTyCorrect(ma.baseExpr->GetTy()) &&
            Is<VArrayTy>(ma.baseExpr->GetTy()) && ma.desugarExpr && ma.desugarExpr->astKind == ASTKind::LIT_CONST_EXPR;
        if (isVArraySize) {
            (void)std::for_each(diags.begin(), diags.end(), [this](auto info) { (void)diag.Diagnose(info); });
            DiagCannotAssignToImmutable(diag, ma, ma);
            return false;
        }
    } else if (e.astKind == ASTKind::SUBSCRIPT_EXPR) {
        auto& se = static_cast<SubscriptExpr&>(e);
        if (!IsAssignableSubScriptExpr(se, isCompound, diags, diag)) {
            return false;
        }
    }
    return true;
}

void TypeChecker::TypeCheckerImpl::DiagnoseForSubscriptAssignExpr(
    ASTContext& ctx, const AssignExpr& ae, std::vector<Diagnostic>& diags)
{
    CJC_NULLPTR_CHECK(ae.leftValue);
    CJC_NULLPTR_CHECK(ae.rightExpr);
    SubscriptExpr& se = StaticCast<SubscriptExpr&>(*ae.leftValue);
    auto baseTarget = se.baseExpr->GetTarget();
    // Try to report optimized diagnostics.
    if (Utils::NotIn(diags, [](const Diagnostic& d) { return d.diagSeverity == DiagSeverity::DS_ERROR; }) ||
        baseTarget == nullptr || baseTarget->astKind == ASTKind::VAR_DECL) {
        // Check whether all the children (`se.indexExprs`, `ae.rightExpr`) are valid.
        bool areChildrenValid = true;
        for (auto& indexExpr : se.indexExprs) {
            CJC_NULLPTR_CHECK(indexExpr);
            areChildrenValid = Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, indexExpr.get())) &&
                ReplaceIdealTy(*indexExpr) && areChildrenValid;
        }
        areChildrenValid = Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, ae.rightExpr.get())) &&
            ReplaceIdealTy(*ae.rightExpr) && areChildrenValid;
        if (areChildrenValid) {
            // If all the children are valid, there must be operator overloading errors.
            // Firstly, report the warnings in children.
            // Then report the subscript assignment operator overloading error.
            DiagCannotAssignToSubscript(diag, se, *ae.rightExpr);
        }
        // Otherwise, some children are invalid, and diagnostics should have been reported
        // by `Synthesize` or `ReplaceIdealTy`.
    } else {
        // Report the stashed diagnostics.
        for (auto& d : diags) {
            (void)diag.Diagnose(d);
        }
    }
}

std::optional<Ptr<Ty>> TypeChecker::TypeCheckerImpl::InferAssignExprCheckCaseOverloading(
    ASTContext& ctx, AssignExpr& ae, std::vector<Diagnostic>& diags)
{
    auto ds = DiagSuppressor(diag);
    if (!ae.desugarExpr && ae.leftValue && ae.leftValue->astKind == ASTKind::SUBSCRIPT_EXPR) {
        SubscriptExpr& se = StaticCast<SubscriptExpr&>(*ae.leftValue);
        DesugarSubscriptOverloadExpr(ctx, ae);
        if (Ty::IsTyCorrect(Synthesize({ctx, SynPos::UNUSED}, ae.desugarExpr.get()))) {
            ds.ReportDiag();
            CJC_NULLPTR_CHECK(ae.desugarExpr);
            ae.SetTy(ae.desugarExpr->GetTy());
            CJC_ASSERT(ae.desugarExpr->astKind == ASTKind::CALL_EXPR);
            ReplaceTarget(&ae, StaticCast<CallExpr*>(ae.desugarExpr.get())->resolvedFunction);
            return {ae.GetTy()};
        }
        RecoverToAssignExpr(ae);
        if (se.baseExpr && MaySubscriptAssignOnlyBeOverload(*se.baseExpr)) {
            auto stashedDiags = ds.GetSuppressedDiag();
            DiagnoseForSubscriptAssignExpr(ctx, ae, stashedDiags);
            ds.ReportDiag();
            return {ae.GetTy()};
        }
        ctx.DeleteDesugarExpr(ae.desugarExpr);
    } else if (ae.isCompound) {
        DesugarOperatorOverloadExpr(ctx, ae);
        if (Ty::IsTyCorrect(Synthesize({ctx, SynPos::UNUSED}, ae.desugarExpr.get()))) {
            ds.ReportDiag();
            ae.SetTy(ae.desugarExpr->GetTy());
            CallExpr& ce = StaticCast<CallExpr&>(*StaticCast<AssignExpr&>(*ae.desugarExpr).rightExpr);
            ReplaceTarget(&ae, ce.resolvedFunction);
            MemberAccess& ma = StaticCast<MemberAccess&>(*ce.baseFunc);
            CJC_ASSERT(ma.baseExpr && ma.baseExpr->GetTy() && !ce.args.empty() && ce.args.front()->GetTy());
            auto iter = COMPOUND_ASSIGN_EXPR_MAP.find(ae.op);
            CJC_ASSERT(iter != COMPOUND_ASSIGN_EXPR_MAP.cend());
            if (IsBuiltinBinaryExpr(iter->second, *ma.baseExpr->GetTy(), *ce.args.front()->GetTy())) {
                RecoverToAssignExpr(ae);
            }
            return {ae.GetTy()};
        }
        RecoverToAssignExpr(ae);
        ctx.DeleteDesugarExpr(ae.desugarExpr);
    }
    diags = ds.GetSuppressedDiag();
    return {};
}

bool TypeChecker::TypeCheckerImpl::ChkAssignExpr(ASTContext& ctx, Ty& target, AssignExpr& ae)
{
    if (!Ty::IsTyCorrect(SynAssignExpr(ctx, ae))) {
        return false;
    }
    Ptr<Ty> unitTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    if (typeManager.IsSubtype(ae.GetTy(), &target)) {
        return true;
    } else {
        DiagMismatchedTypesWithFoundTy(
            diag, ae, target, *unitTy, "the type of an assignment expression is always 'Unit'");
        ae.SetTy(TypeManager::GetInvalidTy());
        return false;
    }
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynMultipleAssignExpr(ASTContext& ctx, AST::AssignExpr& ae)
{
    Synthesize({ctx, SynPos::UNUSED}, ae.desugarExpr.get());
    CJC_NULLPTR_CHECK(ae.desugarExpr);
    ae.SetTy(ae.desugarExpr->GetTy());
    Synthesize({ctx, SynPos::EXPR_ARG}, ae.rightExpr.get());
    CJC_NULLPTR_CHECK(ae.rightExpr);
    if (!Ty::IsTyCorrect(ae.rightExpr->GetTy())) {
        return ae.GetTy();
    }
    Ptr<Ty> rightTy = typeManager.ReplaceIdealTy(ae.rightExpr->GetTy());
    ae.rightExpr->SetTy(rightTy);
    { // Create a scope for DiagSuppressor.
        auto ds = DiagSuppressor(diag);
        Synthesize({ctx, SynPos::LEFT_VALUE}, ae.leftValue.get());
    }
    CheckMultipleAssignExpr(diag, typeManager, ae);
    return ae.GetTy();
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynAssignExpr(ASTContext& ctx, AssignExpr& ae)
{
    if (ae.desugarExpr) {
        // It is a multiple assignment expression.
        if (ae.leftValue && ae.leftValue->astKind == ASTKind::TUPLE_LIT) {
            return SynMultipleAssignExpr(ctx, ae);
        }
        return ae.desugarExpr->GetTy();
    }
    CJC_ASSERT(ae.leftValue && ae.rightExpr);
    ae.SetTy(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));
    if (TryDesugarExternIndexUpdate(typeManager, importManager, ae, [this, &ctx](Ptr<Node> node) {
        auto ty = Synthesize({ctx, SynPos::EXPR_ARG}, node);
        ReplaceIdealTy(*node);
        return ty;
    })) {
        return ae.GetTy();
    }
    std::vector<Diagnostic> diagsForOverload;
    // Check operator overloading for index accessing or compound assignment.
    if (auto ret = InferAssignExprCheckCaseOverloading(ctx, ae, diagsForOverload)) {
        return *ret;
    }
    if (ae.leftValue->astKind == ASTKind::WILDCARD_EXPR) {
        if (Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, ae.rightExpr.get()))) {
            Ptr<Ty> rightTy = typeManager.ReplaceIdealTy(ae.rightExpr->GetTy());
            ae.rightExpr->SetTy(rightTy);
        } else {
            ae.SetTy(TypeManager::GetInvalidTy());
        }
        ae.leftValue->SetTy(ae.rightExpr->GetTy());
        return ae.GetTy();
    }
    auto lTy = Synthesize({ctx, SynPos::LEFT_VALUE}, ae.leftValue.get());
    if (TryDesugarExternMemberUpdate(typeManager, importManager, ae, [this, &ctx](Ptr<Node> node) {
        auto ty = Synthesize({ctx, SynPos::EXPR_ARG}, node);
        ReplaceIdealTy(*node);
        return ty;
    })) {
        return ae.GetTy();
    }
    if (lTy->IsInvalid()) {
        ae.SetTy(TypeManager::GetInvalidTy());
        return TypeManager::GetInvalidTy();
    }
    lTy = Ty::GetPrimitiveUpperBound(lTy);
    if (!IsAssignable(*ae.leftValue, ae.isCompound, diagsForOverload)) {
        return TypeManager::GetInvalidTy();
    }

    // Additional checks for compound assignment expressions:
    // If the assignment operator is not overloaded, only built-in types support compound assign expressions.
    if (!PreCheckCompoundAssign(ctx, ae, *lTy, diagsForOverload)) {
        return TypeManager::GetInvalidTy();
    }

    if (ae.op == TokenKind::EXP_ASSIGN) {
        // Quick fix. Rules for **= are different from all other compound assignment expressions.
        if (!CheckExponentByBaseTy(ctx, *lTy, *ae.leftValue, *ae.rightExpr)) {
            if (ae.ShouldDiagnose()) {
                (void)diag.Diagnose(ae, DiagKind::sema_type_incompatible, "assignment");
            }
            ae.SetTy(TypeManager::GetInvalidTy());
        }
        return ae.GetTy();
    }

    if (!ae.isCompound && ae.leftValue->GetTy()->IsRune() && IsSingleRuneStringLiteral(*ae.rightExpr)) {
        ae.rightExpr->SetTy(ae.leftValue->GetTy());
    } else if (!ae.isCompound && ae.leftValue->TyKind() == TypeKind::TYPE_UINT8 &&
        IsSingleByteStringLiteral(*ae.rightExpr)) {
        ae.rightExpr->SetTy(ae.leftValue->GetTy());
        ChkLitConstExprRange(StaticCast<LitConstExpr&>(*ae.rightExpr));
    } else if (!Check(ctx, lTy, ae.rightExpr.get())) {
        if (ae.ShouldDiagnose() && !CanSkipDiag(*ae.rightExpr)) {
            (void)diag.Diagnose(ae, DiagKind::sema_type_incompatible, "assignment");
        }
        return TypeManager::GetInvalidTy();
    }
    // If the above check passes, ae.rightExpr and ae.rightExpr->GetTy() must not be null.
    // Additional checks for shift assignment expressions: negative left value check and simple overflow check.
    if (!IsShiftAssignValid(ae)) {
        return TypeManager::GetInvalidTy();
    }
    return ae.GetTy();
}

bool TypeChecker::TypeCheckerImpl::IsShiftAssignValid(const AssignExpr& ae)
{
    if (Utils::In(ae.op, SHIFT_ASSIGN_OPERATOR) && ae.rightExpr->astKind == ASTKind::LIT_CONST_EXPR) {
        if (ae.rightExpr->constNumValue.asInt.IsNegativeNum()) {
            (void)diag.Diagnose(*ae.rightExpr, DiagKind::sema_negative_shift_count);
            return false;
        }
        if (Ty::IsTyCorrect(ae.leftValue->GetTy()) && ae.leftValue->GetTy()->IsInteger()) {
            if (ae.rightExpr->constNumValue.asInt.GreaterThanOrEqualBitLen(ae.leftValue->TyKind())) {
                (void)diag.Diagnose(*ae.rightExpr, DiagKind::sema_shift_count_overflow);
                return false;
            }
        }
    }
    return true;
}

bool TypeChecker::TypeCheckerImpl::PreCheckCompoundAssign(
    ASTContext& ctx, const AssignExpr& ae, const Ty& lTy, const std::vector<Diagnostic>& diags)
{
    if (ae.isCompound) {
        const auto& typeCandidates = COMPOUND_ASSIGN_TYPE_MAP.at(ae.op);
        if (Ty::IsInitialTy(&lTy) || !Utils::In(lTy.kind, typeCandidates)) {
            Synthesize({ctx, SynPos::EXPR_ARG}, ae.rightExpr.get());
            if (ae.ShouldDiagnose()) {
                (void)std::for_each(diags.begin(), diags.end(), [this](auto info) { (void)diag.Diagnose(info); });
                (void)diag.Diagnose(ae, DiagKind::sema_type_incompatible, "compound assignment expression");
            }
            return false;
        }
    }
    return true;
}
