// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares the class for the desugaring of Extern<T>.
 */

#ifndef CANGJIE_SEMA_EXTERN_DESUGARING_H
#define CANGJIE_SEMA_EXTERN_DESUGARING_H

#include <functional>
#include <string>
#include <vector>

#include "TypeCheckerImpl.h"

#include "cangjie/AST/Node.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/Sema/TypeManager.h"

namespace Cangjie {
class ExternDesugaring {
public:
    using NeedConversion = std::function<bool(AST::Ty& from, AST::Ty& to)>;
    using RuntimeFuncLookup =
        std::function<ForeignRuntimeFunc(AST::Ty& runtimeTy, const std::string& name, const AST::Expr& expr)>;

    ExternDesugaring(TypeManager& typeManager, NeedConversion needConversion, RuntimeFuncLookup lookup)
        : typeManager(typeManager), needConversion(std::move(needConversion)), lookup(std::move(lookup))
    {
    }

    void Run(AST::Node& root);

private:
    /** A primitive constructor of Extern<T>, with its type instantiated for a given T. */
    struct Ctor {
        Ptr<AST::FuncDecl> decl{nullptr};
        Ptr<AST::FuncTy> ty{nullptr};
    };

    static bool IsDynamic(const AST::Expr& expr);

    // Conversions to Extern<T>.
    bool TryConvert(AST::Expr& expr, AST::Ty& target);
    void TryConvertBlock(AST::Block& block, AST::Ty& target);

    void HandleVarDecl(const AST::VarDecl& vd);
    void HandleAssignExpr(const AST::AssignExpr& ae);
    void HandleCallExpr(AST::CallExpr& ce);
    void HandleReturnExpr(const AST::ReturnExpr& re);
    void HandleFuncBody(const AST::FuncBody& fb);
    void HandleArrayExpr(AST::ArrayExpr& ae);
    void HandleArrayLit(AST::ArrayLit& al);
    void HandleTupleLit(AST::TupleLit& tl);
    void HandleIfExpr(AST::IfExpr& ie);
    void HandleMatchExpr(AST::MatchExpr& me);
    void HandleTryExpr(AST::TryExpr& te);

    // Dynamic operations on Extern<T>.
    void DesugarOperation(AST::Expr& expr);
    OwnedPtr<AST::Expr> BuildTree(AST::Expr& expr);
    OwnedPtr<AST::Expr> BuildOperation(AST::Expr& expr, AST::Ty& externTy);
    // A value selects the update constructor; otherwise these build a read.
    OwnedPtr<AST::Expr> BuildMemberOperation(
        AST::MemberAccess& ma, AST::Ty& externTy, const AST::Expr& source, Ptr<AST::Expr> value = nullptr);
    OwnedPtr<AST::Expr> BuildIndexedOperation(
        AST::SubscriptExpr& se, AST::Ty& externTy, const AST::Expr& source, Ptr<AST::Expr> value = nullptr);
    OwnedPtr<AST::Expr> BuildCallOperation(AST::CallExpr& ce, AST::Ty& externTy);
    OwnedPtr<AST::Expr> BuildAssignmentOperation(AST::AssignExpr& ae, AST::Ty& externTy);
    OwnedPtr<AST::Expr> BuildCompoundAssignment(AST::AssignExpr& ae, AST::Ty& externTy);
    OwnedPtr<AST::Expr> BuildArgs(AST::CallExpr& ce, AST::Ty& arrayTy);
    Ctor LookupCtor(const std::string& name, AST::Ty& externTy);
    static OwnedPtr<AST::CallExpr> CreateCtorCall(
        const Ctor& ctor, AST::Ty& externTy, std::vector<OwnedPtr<AST::Expr>> args, const AST::Expr& expr);

    OwnedPtr<AST::CallExpr> CreateRuntimeCall(const std::string& name, OwnedPtr<AST::Expr> arg, AST::Ty& argTy,
        AST::Ty& externTy, std::vector<Ptr<AST::Ty>> instTys, const AST::Expr& expr);

    TypeManager& typeManager;
    NeedConversion needConversion;
    RuntimeFuncLookup lookup;
};
} // namespace Cangjie

#endif
