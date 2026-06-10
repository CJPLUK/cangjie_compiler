// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "DiagSuppressor.h"
#include "Diags.h"
#include "NativeFFI/Utils.h"
#include "TypeCheckUtil.h"

#include "cangjie/AST/Walker.h"
#include "cangjie/Utils/ConstantsUtils.h"

using namespace Cangjie;
using namespace AST;
using namespace Cangjie::Native::FFI;

namespace {
bool IsConfirmedForcedCastTargetType(const Type& type)
{
    switch (type.astKind) {
        case ASTKind::PRIMITIVE_TYPE:
        case ASTKind::THIS_TYPE:
        case ASTKind::CONSTANT_TYPE:
            return true;
        case ASTKind::INVALID_TYPE:
            return false;
        case ASTKind::REF_TYPE: {
            auto& refType = StaticCast<const RefType&>(type);
            if (!refType.ref.target || !refType.ref.target->IsTypeDecl()) {
                return false;
            }
            return std::all_of(refType.typeArguments.begin(), refType.typeArguments.end(),
                [](const auto& arg) { return arg && IsConfirmedForcedCastTargetType(*arg); });
        }
        case ASTKind::PAREN_TYPE: {
            auto& parenType = StaticCast<const ParenType&>(type);
            return parenType.type && IsConfirmedForcedCastTargetType(*parenType.type);
        }
        case ASTKind::QUALIFIED_TYPE: {
            auto& qualifiedType = StaticCast<const QualifiedType&>(type);
            if (!qualifiedType.target || !qualifiedType.target->IsTypeDecl()) {
                return false;
            }
            return std::all_of(qualifiedType.typeArguments.begin(), qualifiedType.typeArguments.end(),
                [](const auto& arg) { return arg && IsConfirmedForcedCastTargetType(*arg); });
        }
        case ASTKind::OPTION_TYPE: {
            auto& optionType = StaticCast<const OptionType&>(type);
            return optionType.componentType && IsConfirmedForcedCastTargetType(*optionType.componentType);
        }
        case ASTKind::VARRAY_TYPE: {
            auto& varrayType = StaticCast<const VArrayType&>(type);
            return varrayType.typeArgument && IsConfirmedForcedCastTargetType(*varrayType.typeArgument) &&
                varrayType.constantType && IsConfirmedForcedCastTargetType(*varrayType.constantType);
        }
        case ASTKind::FUNC_TYPE: {
            auto& funcType = StaticCast<const FuncType&>(type);
            return funcType.retType && IsConfirmedForcedCastTargetType(*funcType.retType) &&
                std::all_of(funcType.paramTypes.begin(), funcType.paramTypes.end(),
                    [](const auto& arg) { return arg && IsConfirmedForcedCastTargetType(*arg); });
        }
        case ASTKind::TUPLE_TYPE: {
            auto& tupleType = StaticCast<const TupleType&>(type);
            return std::all_of(tupleType.fieldTypes.begin(), tupleType.fieldTypes.end(),
                [](const auto& arg) { return arg && IsConfirmedForcedCastTargetType(*arg); });
        }
        default:
            return false;
    }
}

bool IsExternOperandTy(Ptr<Ty> ty, Ptr<Ty>& runtimeTy)
{
    if (!Ty::IsTyCorrect(ty) || ty->typeArgs.size() != 1) {
        return false;
    }
    auto decl = Ty::GetDeclOfTy(ty);
    if (!decl || decl->identifier.Val() != "Extern" || decl->fullPackageName != INTEROP_PACKAGE_NAME) {
        return false;
    }
    runtimeTy = ty->typeArgs.front();
    return Ty::IsTyCorrect(runtimeTy);
}

bool CanSelectForcedCast(const Type& targetType, Ptr<Ty> targetTy, Ptr<Ty> operandTy, Ptr<Ty>& runtimeTy)
{
    if (!Ty::IsTyCorrect(targetTy) || !IsConfirmedForcedCastTargetType(targetType)) {
        return false;
    }
    return IsExternOperandTy(operandTy, runtimeTy);
}

Ptr<ForcedCastExpr> GetLeadingForcedCastExpr(Ptr<Expr> expr)
{
    if (!expr) {
        return nullptr;
    }
    switch (expr->astKind) {
        case ASTKind::FORCED_CAST_EXPR:
            return StaticAs<ASTKind::FORCED_CAST_EXPR>(expr);
        case ASTKind::BINARY_EXPR:
            return GetLeadingForcedCastExpr(StaticAs<ASTKind::BINARY_EXPR>(expr)->leftExpr.get());
        case ASTKind::ASSIGN_EXPR:
            return GetLeadingForcedCastExpr(StaticAs<ASTKind::ASSIGN_EXPR>(expr)->leftValue.get());
        case ASTKind::RANGE_EXPR:
            return GetLeadingForcedCastExpr(StaticAs<ASTKind::RANGE_EXPR>(expr)->startExpr.get());
        case ASTKind::MEMBER_ACCESS:
            return GetLeadingForcedCastExpr(StaticAs<ASTKind::MEMBER_ACCESS>(expr)->baseExpr.get());
        case ASTKind::SUBSCRIPT_EXPR:
            return GetLeadingForcedCastExpr(StaticAs<ASTKind::SUBSCRIPT_EXPR>(expr)->baseExpr.get());
        case ASTKind::CALL_EXPR:
            return GetLeadingForcedCastExpr(StaticAs<ASTKind::CALL_EXPR>(expr)->baseFunc.get());
        case ASTKind::OPTIONAL_CHAIN_EXPR:
            return GetLeadingForcedCastExpr(StaticAs<ASTKind::OPTIONAL_CHAIN_EXPR>(expr)->expr.get());
        case ASTKind::INC_OR_DEC_EXPR:
            return GetLeadingForcedCastExpr(StaticAs<ASTKind::INC_OR_DEC_EXPR>(expr)->expr.get());
        default:
            return nullptr;
    }
}

void AdoptChosenExpr(OwnedPtr<Expr>& chosenExpr, Expr& sourceExpr)
{
    if (!chosenExpr) {
        return;
    }
    chosenExpr->sourceExpr = Ptr(&sourceExpr);
    chosenExpr->curFile = sourceExpr.curFile;
}

void PropagateGeneratedContext(const Expr& sourceExpr, Node& candidate)
{
    auto visit = [&sourceExpr](Ptr<Node> node) {
        node->scopeName = sourceExpr.scopeName;
        node->scopeLevel = sourceExpr.scopeLevel;
        node->curFile = sourceExpr.curFile;
        return VisitAction::WALK_CHILDREN;
    };
    Walker walker(&candidate, visit);
    walker.Walk();
}

OwnedPtr<CallExpr> BuildForcedCastCall(
    Expr& sourceExpr, Type& targetTypeAst, Ty& targetTy, Ty& runtimeTy, Expr& operandExpr, TypeManager& typeManager)
{
    auto runtimeDecl = Ty::GetDeclOfTy(&runtimeTy);
    if (!runtimeDecl) {
        return nullptr;
    }

    auto runtimeRef = CreateRefExpr(*runtimeDecl, sourceExpr);
    runtimeRef->curFile = sourceExpr.curFile;
    runtimeRef->isAlone = false;
    auto fromExternDecl =
        TypeCheckUtil::GetMemberDecl<FuncDecl>(*runtimeDecl, "fromExtern", {operandExpr.GetTy()}, typeManager);
    if (!fromExternDecl) {
        return nullptr;
    }
    auto operandClone = ASTCloner::Clone(Ptr(&operandExpr));
    operandClone->curFile = sourceExpr.curFile;
    auto callExpr = CreateMemberCall(std::move(runtimeRef), fromExternDecl, std::move(operandClone));
    if (!callExpr) {
        return nullptr;
    }
    auto memberAccess = DynamicCast<MemberAccess*>(callExpr->baseFunc.get());
    CJC_NULLPTR_CHECK(memberAccess);
    // Keep the generated call structurally aligned with an explicitly written
    // `Runtime.fromExtern<U>(e)` call: later CHIR/codegen uses the callee type,
    // not just the final CallExpr type.
    memberAccess->SetTy(typeManager.GetFunctionTy({operandExpr.GetTy()}, &targetTy));
    auto targetTypeClone = ASTCloner::Clone(Ptr(&targetTypeAst));
    if (!targetTypeClone) {
        return nullptr;
    }
    memberAccess->instTys.push_back(&targetTy);
    memberAccess->typeArguments.push_back(std::move(targetTypeClone));
    callExpr->resolvedFunction = fromExternDecl;
    callExpr->callKind = CallKind::CALL_DECLARED_FUNCTION;
    callExpr->SetTy(&targetTy);
    callExpr->sourceExpr = &sourceExpr;
    callExpr->begin = sourceExpr.begin;
    callExpr->end = sourceExpr.end;
    PropagateGeneratedContext(sourceExpr, *callExpr);
    return callExpr;
}

void PropagateAmbiguousCandidateContext(const Expr& sourceExpr, Node& candidate)
{
    auto visit = [&sourceExpr](Ptr<Node> node) {
        node->scopeName = sourceExpr.scopeName;
        node->scopeLevel = sourceExpr.scopeLevel;
        node->curFile = sourceExpr.curFile;
        return VisitAction::WALK_CHILDREN;
    };
    Walker walker(&candidate, visit);
    walker.Walk();
}
} // namespace

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynForcedCastExpr(ASTContext& ctx, ForcedCastExpr& fce)
{
    CJC_NULLPTR_CHECK(fce.targetType);
    CJC_NULLPTR_CHECK(fce.expr);
    fce.targetType->SetTy(Synthesize({ctx, SynPos::NONE}, fce.targetType.get()));
    auto targetTy = fce.targetType->GetTy();
    auto operandTy = Synthesize({ctx, SynPos::EXPR_ARG}, fce.expr.get());
    Ptr<Ty> runtimeTy = TypeManager::GetInvalidTy();
    if (!CanSelectForcedCast(*fce.targetType, targetTy, operandTy, runtimeTy)) {
        if (Ty::IsTyCorrect(targetTy) && Ty::IsTyCorrect(operandTy)) {
            diag.Diagnose(fce, DiagKind::sema_invalid_forced_cast_expr);
        }
        fce.SetTy(TypeManager::GetInvalidTy());
        return fce.GetTy();
    }

    fce.desugarExpr = BuildForcedCastCall(fce, *fce.targetType, *targetTy, *runtimeTy, *fce.expr, typeManager);
    if (!fce.desugarExpr) {
        fce.SetTy(TypeManager::GetInvalidTy());
        return fce.GetTy();
    }

    auto desugaredTy = SynthesizeWithoutRecover({ctx, SynPos::EXPR_ARG}, fce.desugarExpr.get());
    if (!Ty::IsTyCorrect(desugaredTy)) {
        fce.SetTy(TypeManager::GetInvalidTy());
        return fce.GetTy();
    }
    fce.SetTy(targetTy);
    return fce.GetTy();
}

bool TypeChecker::TypeCheckerImpl::ChkForcedCastExpr(ASTContext& ctx, Ty& targetTy, ForcedCastExpr& fce)
{
    if (Ty::IsTyCorrect(SynForcedCastExpr(ctx, fce)) && typeManager.IsSubtype(fce.GetTy(), &targetTy)) {
        return true;
    }
    if (!TypeCheckUtil::CanSkipDiag(fce)) {
        Sema::DiagMismatchedTypes(diag, fce, targetTy);
    }
    fce.SetTy(TypeManager::GetInvalidTy());
    return false;
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynAmbiguousForcedCastExpr(ASTContext& ctx, AmbiguousForcedCastExpr& afce)
{
    CJC_NULLPTR_CHECK(afce.forcedExpr);
    CJC_NULLPTR_CHECK(afce.fallbackExpr);
    auto fce = GetLeadingForcedCastExpr(afce.forcedExpr.get());
    CJC_NULLPTR_CHECK(fce);
    CJC_NULLPTR_CHECK(fce->targetType);
    CJC_NULLPTR_CHECK(fce->expr);

    Ptr<Ty> targetTy = TypeManager::GetInvalidTy();
    Ptr<Ty> operandTy = TypeManager::GetInvalidTy();
    Ptr<Ty> runtimeTy = TypeManager::GetInvalidTy();
    PropagateAmbiguousCandidateContext(afce, *afce.forcedExpr);
    {
        DiagSuppressor ds(diag);
        SetTypeTy(ctx, *fce->targetType);
        targetTy = fce->targetType->GetTy();
        if (Ty::IsTyCorrect(targetTy)) {
            operandTy = Synthesize({ctx, SynPos::EXPR_ARG}, fce->expr.get());
            (void)CanSelectForcedCast(*fce->targetType, targetTy, operandTy, runtimeTy);
        }
        (void)ds.GetSuppressedDiag();
    }
    if (CanSelectForcedCast(*fce->targetType, targetTy, operandTy, runtimeTy)) {
        auto forcedTy = SynthesizeWithoutRecover({ctx, SynPos::EXPR_ARG}, afce.forcedExpr.get());
        afce.SetTy(forcedTy);
        if (Ty::IsTyCorrect(forcedTy)) {
            afce.desugarExpr = std::move(afce.forcedExpr);
            AdoptChosenExpr(afce.desugarExpr, afce);
        }
        return afce.GetTy();
    }

    auto fallbackTy = Synthesize({ctx, SynPos::EXPR_ARG}, afce.fallbackExpr.get());
    afce.SetTy(fallbackTy);
    if (Ty::IsTyCorrect(fallbackTy)) {
        afce.desugarExpr = std::move(afce.fallbackExpr);
        AdoptChosenExpr(afce.desugarExpr, afce);
    }
    return afce.GetTy();
}

bool TypeChecker::TypeCheckerImpl::ChkAmbiguousForcedCastExpr(ASTContext& ctx, Ty& targetTy, AmbiguousForcedCastExpr& afce)
{
    CJC_NULLPTR_CHECK(afce.forcedExpr);
    CJC_NULLPTR_CHECK(afce.fallbackExpr);
    auto fce = GetLeadingForcedCastExpr(afce.forcedExpr.get());
    CJC_NULLPTR_CHECK(fce);
    CJC_NULLPTR_CHECK(fce->targetType);
    CJC_NULLPTR_CHECK(fce->expr);

    Ptr<Ty> targetTyCandidate = TypeManager::GetInvalidTy();
    Ptr<Ty> operandTy = TypeManager::GetInvalidTy();
    Ptr<Ty> runtimeTy = TypeManager::GetInvalidTy();
    PropagateAmbiguousCandidateContext(afce, *afce.forcedExpr);
    {
        DiagSuppressor ds(diag);
        SetTypeTy(ctx, *fce->targetType);
        targetTyCandidate = fce->targetType->GetTy();
        if (Ty::IsTyCorrect(targetTyCandidate)) {
            operandTy = Synthesize({ctx, SynPos::EXPR_ARG}, fce->expr.get());
            (void)CanSelectForcedCast(*fce->targetType, targetTyCandidate, operandTy, runtimeTy);
        }
        (void)ds.GetSuppressedDiag();
    }
    if (CanSelectForcedCast(*fce->targetType, targetTyCandidate, operandTy, runtimeTy)) {
        bool ok = Check(ctx, &targetTy, afce.forcedExpr.get());
        if (ok) {
            afce.SetTy(afce.forcedExpr->GetTy());
            afce.desugarExpr = std::move(afce.forcedExpr);
            AdoptChosenExpr(afce.desugarExpr, afce);
            return true;
        }
        afce.SetTy(TypeManager::GetInvalidTy());
        return false;
    }

    bool ok = Check(ctx, &targetTy, afce.fallbackExpr.get());
    if (ok) {
        afce.SetTy(afce.fallbackExpr->GetTy());
        afce.desugarExpr = std::move(afce.fallbackExpr);
        AdoptChosenExpr(afce.desugarExpr, afce);
    } else {
        afce.desugarExpr = nullptr;
        afce.SetTy(TypeManager::GetInvalidTy());
    }
    return ok;
}
