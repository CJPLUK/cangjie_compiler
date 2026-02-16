// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeCheckerImpl.h"

using namespace Cangjie;
using namespace AST;

Ptr<Ty> TypeChecker::TypeCheckerImpl::SynResumeExpr(ASTContext& ctx, ResumeExpr& re)
{
    Ty* resumptionParamTy = typeManager.GetAnyTy();
    if (re.expr) {
        // Resuming a deferred resumption - get result and return type from Resumption<Res, Ret>
        CJC_NULLPTR_CHECK(re.expr);
        auto exprTy = Synthesize({ctx, SynPos::NONE}, re.expr.get());
        if (!Ty::IsTyCorrect(exprTy)) {
            return TypeManager::GetInvalidTy();
        }
        auto resumption = importManager.GetImportedDecl(EFFECT_PACKAGE_NAME, CLASS_RESUMPTION);
        std::set<Ptr<Ty>> exprAsResumptionTy;
        if (resumption) {
            exprAsResumptionTy = promotion.Promote(*exprTy, *resumption->ty);
        }
        if (!resumption || exprAsResumptionTy.size() != 1) {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_resume_wrong_resumption_type, *re.expr, exprTy->String());
            re.ty = TypeManager::GetInvalidTy();
            return re.ty;
        }
        resumptionParamTy = (*exprAsResumptionTy.begin())->typeArgs[0];
        re.ty = (*exprAsResumptionTy.begin())->typeArgs[1];
    } else {
        if (re.enclosing) {
            resumptionParamTy = (*re.enclosing)->commandResultTy;
            re.ty = TypeManager::GetNothingTy();
        } else {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_implicit_resume_outside_handler, re);
            re.ty = TypeManager::GetInvalidTy();
        }
    }
    if (re.throwingExpr) {
        auto exception = importManager.GetCoreDecl<ClassDecl>(CLASS_EXCEPTION);
        auto error = importManager.GetCoreDecl<ClassDecl>(CLASS_ERROR);
        Synthesize({ctx, SynPos::EXPR_ARG}, re.throwingExpr.get());
        CJC_NULLPTR_CHECK(re.throwingExpr->ty);

        if (!typeManager.IsSubtype(re.throwingExpr->ty, exception->ty) &&
            !typeManager.IsSubtype(re.throwingExpr->ty, error->ty)) {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_resume_throwing_mismatch_type, re);
            re.ty = TypeManager::GetInvalidTy();
        }
    } else if (re.withExpr) {
        if (!Check(ctx, resumptionParamTy, re.withExpr)) {
            // `Check` produces the error message.
            re.ty = TypeManager::GetInvalidTy();
        }
    } else {
        auto unitTy = typeManager.GetPrimitiveTy(TypeKind::TYPE_UNIT);
        if (!typeManager.IsSubtype(resumptionParamTy, unitTy)) {
            diag.DiagnoseRefactor(DiagKindRefactor::sema_resume_no_with, re, Ty::ToString(resumptionParamTy));
            re.ty = TypeManager::GetInvalidTy();
        }
    }
    return re.ty;
}
