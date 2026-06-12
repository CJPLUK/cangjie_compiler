// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

#include "Desugar/DesugarInTypeCheck.h"
#include "DiagSuppressor.h"
#include "Diags.h"
#include "TypeCheckUtil.h"

#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/RecoverDesugar.h"

using namespace Cangjie;
using namespace Sema;
using namespace TypeCheckUtil;

namespace {
template <typename F>
bool TryDesugarExternIndexAccess(
    TypeManager& typeManager, ImportManager& importManager, SubscriptExpr& se, F typecheck)
{
    CJC_NULLPTR_CHECK(se.baseExpr);
    auto sourceExternTy = se.baseExpr->GetTy();
    if (!TypeIsExtern(importManager, sourceExternTy)) {
        return false;
    }
    if (se.TestAttr(Attribute::LEFT_VALUE)) {
        se.SetTy(sourceExternTy);
        return true;
    }

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

    OwnedPtr<Expr> indexedExpr = ASTCloner::Clone(Ptr(se.baseExpr.get()));
    for (auto& indexExpr : se.indexExprs) {
        auto runtimeRef = CreateRefExpr(*runtimeDecl);
        runtimeRef->isAlone = false;
        runtimeRef->SetTy(runtimeTy);
        runtimeRef->EnableAttr(Attribute::COMPILER_ADD);
        CopyBasicInfo(&se, runtimeRef.get());

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
        CopyBasicInfo(&se, indexAccess.get());
        if (!indexAccessDecl) {
            se.SetTy(TypeManager::GetInvalidTy());
            return true;
        }
        indexAccess->targets.emplace_back(indexAccessDecl);

        std::vector<OwnedPtr<FuncArg>> args;
        args.emplace_back(CreateFuncArg(std::move(indexedExpr)));
        args.emplace_back(CreateFuncArg(ASTCloner::Clone(Ptr(indexExpr.get()))));

        auto callTarget = isGenericRuntimeTy ? Ptr<FuncDecl>() : indexAccessDecl;
        auto call = CreateCallExpr(
            std::move(indexAccess), std::move(args), callTarget, sourceExternTy, CallKind::CALL_DECLARED_FUNCTION);
        CopyBasicInfo(&se, call.get());
        call->sourceExpr = &se;
        call->EnableAttr(Attribute::COMPILER_ADD);
        call->SetTy(sourceExternTy);
        indexedExpr = std::move(call);
    }

    Ptr<Ty> callTy = typecheck(indexedExpr.get());
    auto call = DynamicCast<CallExpr*>(indexedExpr.get());
    bool hasExpectedTarget = isGenericRuntimeTy ||
        (call && call->resolvedFunction && call->resolvedFunction->identifier == "indexAccess");
    bool isOk = Ty::IsTyCorrect(callTy) && call && hasExpectedTarget && typeManager.IsSubtype(call->GetTy(), sourceExternTy);

    Ptr<Ty> resultTy = isOk ? Ptr<Ty>(sourceExternTy) : Ptr<Ty>(TypeManager::GetInvalidTy());
    se.SetTy(resultTy);
    indexedExpr->SetTy(resultTy);
    if (isOk) {
        se.desugarExpr = std::move(indexedExpr);
    }
    return true;
}
} // namespace

bool TypeChecker::TypeCheckerImpl::ChkSubscriptExpr(ASTContext& ctx, Ptr<Ty> target, SubscriptExpr& se)
{
    if (se.desugarExpr) {
        if (target == nullptr) {
            se.SetTy(se.desugarExpr->GetTy());
            return Ty::IsTyCorrect(se.GetTy());
        }
        bool isWellTyped = typeManager.IsSubtype(se.desugarExpr->GetTy(), target);
        se.SetTy(isWellTyped ? se.desugarExpr->GetTy() : TypeManager::GetInvalidTy());
        return isWellTyped;
    }
    se.SetTy(TypeManager::GetInvalidTy()); // Set invalid ty at first, will be updated later.
    bool invalid = !se.baseExpr || se.indexExprs.empty();
    if (invalid) {
        return false;
    }
    SetIsNotAlone(*se.baseExpr);
    Ptr<Ty> baseTy = Synthesize({ctx, SynPos::EXPR_ARG}, se.baseExpr.get());
    std::vector<Ptr<Ty>> indexTys{};
    for (auto& expr : se.indexExprs) {
        indexTys.push_back(Synthesize({ctx, SynPos::EXPR_ARG}, expr.get()));
    }
    if (!Ty::IsTyCorrect(baseTy) || !Ty::AreTysCorrect(indexTys)) {
        return false;
    }
    if (TryDesugarExternIndexAccess(typeManager, importManager, se, [this, &ctx, target](Ptr<Node> node) {
        if (target == nullptr) {
            return Synthesize({ctx, SynPos::EXPR_ARG}, node);
        }
        (void)Check(ctx, target, node);
        return node->GetTy();
    })) {
        return Ty::IsTyCorrect(se.GetTy());
    }
    // NOTE: Tuple and VArray type support built-in 'SubscriptExpr', others are all operator overload.
    if (auto tupleTy = DynamicCast<TupleTy*>(baseTy); tupleTy && se.indexExprs.size() == 1) {
        se.isTupleAccess = true;
        return ChkTupleAccess(ctx, target, se, *tupleTy);
    }
    if (auto varrTy = DynamicCast<VArrayTy*>(baseTy); varrTy) {
        return ChkVArrayAccess(ctx, target, se, *varrTy);
    }
    auto ds = DiagSuppressor(diag);
    DesugarOperatorOverloadExpr(ctx, se); // Desugar to callExpr.
    // The type of baseExpr should not be inferred here!
    bool isWellTyped = target == nullptr
        ? Ty::IsTyCorrect(Synthesize({ctx, SynPos::EXPR_ARG}, se.desugarExpr.get()))
        : Check(ctx, target, se.desugarExpr.get());
    if (isWellTyped) {
        ds.ReportDiag();
        se.SetTy(se.desugarExpr->GetTy());
        CJC_ASSERT(se.desugarExpr->astKind == ASTKind::CALL_EXPR);
        auto desugaredCE = StaticAs<ASTKind::CALL_EXPR>(se.desugarExpr.get());
        CJC_ASSERT(desugaredCE->resolvedFunction != nullptr);
        ReplaceTarget(&se, desugaredCE->resolvedFunction);
        return true;
    }
    auto synTy = Synthesize({ctx, SynPos::EXPR_ARG}, se.desugarExpr.get());
    bool retTyMismatch = Ty::IsTyCorrect(target) && Ty::IsTyCorrect(synTy);
    RecoverToSubscriptExpr(se);
    // Also recover base and index's type.
    baseTy = typeManager.ReplaceIdealTy(std::move(baseTy));
    se.baseExpr->SetTy(se.baseExpr->GetTy() ? se.baseExpr->GetTy() : baseTy);
    for (size_t i = 0; i < se.indexExprs.size(); ++i) {
        indexTys[i] = typeManager.ReplaceIdealTy(std::move(indexTys[i]));
        se.indexExprs[i]->SetTy(Ty::IsTyCorrect(se.indexExprs[i]->GetTy()) ? se.indexExprs[i]->GetTy() : indexTys[i]);
    }
    if (!ds.HasError() && se.ShouldDiagnose(true)) { // Only report subscript diagnoses if no error has beed reported.
        ds.ReportDiag(); // Report warnings.
        if (retTyMismatch) {
            CJC_NULLPTR_CHECK(target);
            DiagMismatchedTypesWithFoundTy(diag, se, *target, *synTy);
        } else {
            DiagInvalidSubscriptExpr(diag, se, *baseTy, indexTys);
        }
    }
    ds.ReportDiag();
    return false;
}

bool TypeChecker::TypeCheckerImpl::ChkTupleAccess(ASTContext& ctx, Ptr<Ty> target, SubscriptExpr& se, TupleTy& tupleTy)
{
    if (se.baseExpr == nullptr || se.indexExprs.size() != 1) {
        return false;
    }
    if (se.indexExprs[0]->astKind != ASTKind::LIT_CONST_EXPR) {
        diag.Diagnose(*se.indexExprs[0], DiagKind::sema_builtin_invalid_index, "tuple");
        return false;
    }
    if (!Check(ctx, TypeManager::GetPrimitiveTy(TypeKind::TYPE_INT64), se.indexExprs[0].get())) {
        diag.Diagnose(*se.indexExprs[0], DiagKind::sema_builtin_invalid_index, "tuple");
        return false;
    }
    auto idxExpr = StaticAs<ASTKind::LIT_CONST_EXPR>(se.indexExprs[0].get());
    uint64_t index = idxExpr->constNumValue.asInt.Uint64();
    if (index >= tupleTy.typeArgs.size()) {
        if (se.ShouldDiagnose(true)) {
            diag.Diagnose(*se.indexExprs[0], DiagKind::sema_builtin_index_in_bound, "tuple");
        }
        return false;
    }
    if (target == nullptr) { // Type inferring.
        se.SetTy(tupleTy.typeArgs[index]);
        return true;
    }
    if (auto tl = AST::As<ASTKind::TUPLE_LIT>(se.baseExpr.get()); tl) {
        CJC_ASSERT(index < tl->children.size());
        // Update tuple ty if baseExpr is a TupleLit whenever check succeed or failed,
        // because the check result will affect child expr of tuple lit which
        // may changing ideal ty to exact ty or changing valid ty to invalid ty.
        if (!Check(ctx, target, tl->children[index].get())) {
            // Reset the ty of tuple lit to allow re-synthesize of the tuple lit.
            tl->SetTy(TypeManager::GetInvalidTy());
            return false;
        }
        std::vector<Ptr<Ty>> elemTy;
        for (auto& it : tl->children) {
            if (it != nullptr && ReplaceIdealTy(*it)) {
                elemTy.emplace_back(it->GetTy());
            } else {
                elemTy.emplace_back(TypeManager::GetInvalidTy());
            }
        }
        tl->SetTy(typeManager.GetTupleTy(elemTy));
        se.SetTy(elemTy[index]);
    } else {
        if (!typeManager.IsSubtype(tupleTy.typeArgs[index], target)) {
            DiagMismatchedTypesWithFoundTy(diag, se, target->String(), tupleTy.typeArgs[index]->String());
            return false;
        }
        se.SetTy(tupleTy.typeArgs[index]);
    }
    return true;
}

bool TypeChecker::TypeCheckerImpl::ChkVArrayAccess(ASTContext& ctx, Ptr<Ty> target, SubscriptExpr& se, VArrayTy& varrTy)
{
    if (se.indexExprs.size() != 1) {
        diag.DiagnoseRefactor(DiagKindRefactor::sema_varray_subscript_num, se);
        return false;
    }
    {
        DiagSuppressor ds(diag); // only for examing missing error report
        auto i64 = TypeManager::GetPrimitiveTy(TypeKind::TYPE_INT64);
        auto idxExpr = se.indexExprs[0].get();
        if (!Check(ctx, i64, idxExpr)) {
            if (!ds.HasError()) {
                DiagMismatchedTypesWithFoundTy(diag, *idxExpr, i64->String(), idxExpr->GetTy()->String());
            }
            ds.ReportDiag();
            return false;
        }
        ds.ReportDiag();
    }
    CJC_ASSERT(!varrTy.typeArgs.empty() && varrTy.typeArgs[0]);
    if (target == nullptr) { // Type inferring.
        se.SetTy(varrTy.typeArgs[0]);
        return true;
    }
    // check subvalue type.
    if (!typeManager.IsSubtype(varrTy.typeArgs[0], target)) {
        DiagMismatchedTypesWithFoundTy(diag, se, target->String(), varrTy.typeArgs[0]->String());
        return false;
    }
    se.SetTy(varrTy.typeArgs[0]);
    return true;
}

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynSubscriptExpr(ASTContext& ctx, SubscriptExpr& se)
{
    ChkSubscriptExpr(ctx, nullptr, se);
    return se.GetTy();
}
