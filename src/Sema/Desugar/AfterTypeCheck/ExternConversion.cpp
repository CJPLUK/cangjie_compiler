// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the desugaring of implicit conversions to Extern<T>: an expression e of type U used where
 * Extern<T> is expected is rewritten into T.toExtern<U>(e).
 */

#include "TypeCheckerImpl.h"

#include "Desugar/AfterTypeCheck.h"
#include "TypeCheckUtil.h"

#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Utils.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/Basic/Match.h"

using namespace Cangjie;
using namespace AST;
using namespace Meta;
using namespace TypeCheckUtil;

namespace Cangjie::Sema::Desugar::AfterTypeCheck {
OwnedPtr<MemberAccess> CreateForeignRuntimeFuncAccess(
    Ty& runtimeTy, FuncDecl& func, Ptr<Ty> matchedParentTy, Ty& funcTy, const Node& pos)
{
    Ptr<Decl> runtimeDecl = runtimeTy.IsGeneric() ? Ptr<Decl>(StaticCast<GenericsTy*>(&runtimeTy)->decl)
                                                  : Ty::GetDeclOfTy(&runtimeTy);
    if (!runtimeDecl) {
        return nullptr;
    }
    auto runtimeRef = CreateRefExpr(*runtimeDecl);
    runtimeRef->SetTy(&runtimeTy);
    runtimeRef->isAlone = false;
    if (!runtimeTy.IsGeneric()) {
        runtimeRef->instTys = runtimeTy.typeArgs;
    }
    CopyBasicInfo(&pos, runtimeRef.get());

    auto funcAccess = CreateMemberAccess(std::move(runtimeRef), func);
    funcAccess->EnableAttr(Attribute::IMPLICIT_ADD);
    funcAccess->matchedParentTy = matchedParentTy;
    funcAccess->SetTy(&funcTy);
    return funcAccess;
}
} // namespace Cangjie::Sema::Desugar::AfterTypeCheck

namespace {
const std::string TO_EXTERN_FUNC = "toExtern";

class ExternConversion {
public:
    using NeedConversion = std::function<bool(Ty& from, Ty& to)>;
    using TargetLookup = std::function<ForeignRuntimeFunc(Ty& runtimeTy, Ptr<const File> file)>;

    ExternConversion(TypeManager& typeManager, NeedConversion needConversion, TargetLookup lookup)
        : typeManager(typeManager), needConversion(std::move(needConversion)), lookup(std::move(lookup))
    {
    }

    void Run(Package& pkg)
    {
        std::function<VisitAction(Ptr<Node>)> preVisit = [this](Ptr<Node> node) -> VisitAction {
            return match(*node)([this](const VarDecl& vd) { return HandleVarDecl(vd); },
                [this](const AssignExpr& ae) { return HandleAssignExpr(ae); },
                [this](CallExpr& ce) { return HandleCallExpr(ce); },
                [this](const ReturnExpr& re) { return HandleReturnExpr(re); },
                [this](const FuncBody& fb) { return HandleFuncBody(fb); },
                [this](ArrayExpr& ae) { return HandleArrayExpr(ae); },
                []() { return VisitAction::WALK_CHILDREN; });
        };
        Walker walker(&pkg, preVisit);
        walker.Walk();
    }

private:
    bool TryConvert(Expr& expr, Ty& target);
    void TryConvertBlock(Block& block, Ty& target);
    OwnedPtr<Expr> CreateToExternCall(OwnedPtr<Expr> value, Ty& valueTy, Ty& target, Ptr<const File> file);

    VisitAction HandleVarDecl(const VarDecl& vd);
    VisitAction HandleAssignExpr(const AssignExpr& ae);
    VisitAction HandleCallExpr(CallExpr& ce);
    VisitAction HandleReturnExpr(const ReturnExpr& re);
    VisitAction HandleFuncBody(const FuncBody& fb);
    VisitAction HandleArrayExpr(ArrayExpr& ae);

    TypeManager& typeManager;
    NeedConversion needConversion;
    TargetLookup lookup;
};

OwnedPtr<Expr> ExternConversion::CreateToExternCall(
    OwnedPtr<Expr> value, Ty& valueTy, Ty& target, Ptr<const File> file)
{
    CJC_ASSERT(target.IsCoreExternType());
    auto runtimeTy = target.typeArgs[0];
    auto [toExtern, matchedParentTy] = lookup(*runtimeTy, file);
    if (!toExtern) {
        return nullptr;
    }
    auto baseFunc = Sema::Desugar::AfterTypeCheck::CreateForeignRuntimeFuncAccess(
        *runtimeTy, *toExtern, matchedParentTy, *typeManager.GetFunctionTy({&valueTy}, &target), *value);
    if (!baseFunc) {
        return nullptr;
    }
    baseFunc->instTys = {&valueTy};

    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateFuncArg(std::move(value)));
    auto ce = CreateCallExpr(std::move(baseFunc), std::move(args), toExtern, &target, CallKind::CALL_DECLARED_FUNCTION);
    ce->EnableAttr(Attribute::IMPLICIT_ADD);
    return ce;
}

bool ExternConversion::TryConvert(Expr& expr, Ty& target)
{
    auto valueTy = expr.GetTy();
    if (!valueTy || !needConversion(*valueTy, target)) {
        return false;
    }
    auto value = expr.desugarExpr ? std::move(expr.desugarExpr) : ASTCloner::Clone(Ptr(&expr));
    auto ce = CreateToExternCall(std::move(value), *valueTy, target, expr.curFile);
    if (!ce) {
        return false;
    }
    CopyBasicInfo(&expr, ce.get());
    if (expr.astKind == ASTKind::BLOCK) {
        // For correct deserialization, we need to keep type of block.
        auto b = MakeOwnedNode<Block>();
        b->SetTy(ce->GetTy());
        b->body.emplace_back(std::move(ce));
        expr.desugarExpr = std::move(b);
    } else {
        expr.desugarExpr = std::move(ce);
    }
    AddCurFile(*expr.desugarExpr, expr.curFile);
    expr.SetTy(expr.desugarExpr->GetTy());
    return true;
}

void ExternConversion::TryConvertBlock(Block& block, Ty& target)
{
    // If the block is empty or ends with a declaration, its value is the unit value.
    auto lastExprOrDecl = block.GetLastExprOrDecl();
    Ptr<Ty> lastTy = TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT);
    if (auto expr = DynamicCast<Expr*>(lastExprOrDecl)) {
        lastTy = expr->GetTy();
    }
    if (!lastTy || !needConversion(*lastTy, target)) {
        return;
    }
    if (Is<Decl>(lastExprOrDecl) || block.body.empty()) {
        auto unitExpr = CreateUnitExpr(TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));
        unitExpr->curFile = block.curFile;
        lastExprOrDecl = unitExpr.get();
        block.body.emplace_back(std::move(unitExpr));
    }
    if (auto lastExpr = DynamicCast<Expr*>(lastExprOrDecl)) {
        TryConvert(*lastExpr, target);
        block.SetTy(lastExpr->GetTy());
    }
}

VisitAction ExternConversion::HandleVarDecl(const VarDecl& vd)
{
    if (vd.initializer && vd.GetTy()) {
        TryConvert(*vd.initializer, *vd.GetTy());
    }
    return VisitAction::WALK_CHILDREN;
}

VisitAction ExternConversion::HandleAssignExpr(const AssignExpr& ae)
{
    if (ae.desugarExpr || ae.isCompound || !ae.leftValue || !ae.rightExpr || !ae.leftValue->GetTy()) {
        return VisitAction::WALK_CHILDREN;
    }
    TryConvert(*ae.rightExpr, *ae.leftValue->GetTy());
    return VisitAction::WALK_CHILDREN;
}

VisitAction ExternConversion::HandleCallExpr(CallExpr& ce)
{
    if (!ce.baseFunc || !Ty::IsTyCorrect(ce.baseFunc->GetTy()) || !ce.baseFunc->GetTy()->IsFunc()) {
        return VisitAction::WALK_CHILDREN;
    }
    auto funcTy = RawStaticCast<FuncTy*>(ce.baseFunc->GetTy());
    auto convertArgs = [this, funcTy](auto begin, auto end) {
        size_t index = 0;
        for (auto it = begin; it != end && index < funcTy->paramTys.size(); ++it, ++index) {
            auto paramTy = funcTy->paramTys[index];
            if ((*it)->expr && paramTy && TryConvert(*(*it)->expr, *paramTy)) {
                (*it)->SetTy((*it)->expr->GetTy());
            }
        }
    };
    if (ce.desugarArgs.has_value()) {
        convertArgs(ce.desugarArgs->begin(), ce.desugarArgs->end());
    } else {
        convertArgs(ce.args.begin(), ce.args.end());
    }
    return VisitAction::WALK_CHILDREN;
}

VisitAction ExternConversion::HandleReturnExpr(const ReturnExpr& re)
{
    if (!re.expr || !re.refFuncBody || !Ty::IsTyCorrect(re.refFuncBody->GetTy()) ||
        !re.refFuncBody->GetTy()->IsFunc()) {
        return VisitAction::WALK_CHILDREN;
    }
    auto retTy = RawStaticCast<FuncTy*>(re.refFuncBody->GetTy())->retTy;
    if (retTy) {
        TryConvert(re.desugarExpr ? *re.desugarExpr : *re.expr, *retTy);
    }
    return VisitAction::WALK_CHILDREN;
}

VisitAction ExternConversion::HandleFuncBody(const FuncBody& fb)
{
    if (!fb.body || !Ty::IsTyCorrect(fb.GetTy()) || !fb.GetTy()->IsFunc()) {
        return VisitAction::WALK_CHILDREN;
    }
    auto retTy = RawStaticCast<FuncTy*>(fb.GetTy())->retTy;
    if (retTy && retTy->IsCoreExternType()) {
        TryConvertBlock(*fb.body, *retTy);
    }
    return VisitAction::WALK_CHILDREN;
}

VisitAction ExternConversion::HandleArrayExpr(ArrayExpr& ae)
{
    if (!Ty::IsTyCorrect(ae.GetTy()) || ae.initFunc || ae.args.empty()) {
        return VisitAction::WALK_CHILDREN;
    }
    auto typeArgs = typeManager.GetTypeArgs(*ae.GetTy());
    if (typeArgs.empty() || !typeArgs[0]) {
        return VisitAction::WALK_CHILDREN;
    }
    // VArray takes the element as its only argument, Array(size, item: T) as its second one.
    Ptr<FuncArg> arg = ae.isValueArray ? ae.args[0].get() : (ae.args.size() > 1 ? ae.args[1].get() : nullptr);
    if (arg && arg->expr && TryConvert(*arg->expr, *typeArgs[0])) {
        arg->SetTy(arg->expr->GetTy());
    }
    return VisitAction::WALK_CHILDREN;
}
} // namespace

ForeignRuntimeFunc TypeChecker::TypeCheckerImpl::LookupForeignRuntimeFunc(
    ASTContext& ctx, Ty& runtimeTy, const std::string& name, Ptr<const File> file)
{
    std::vector<Ptr<Decl>> candidates;
    if (runtimeTy.IsGeneric()) {
        auto foreignRuntime = importManager.GetCoreDecl<InterfaceDecl>(STD_LIB_FOREIGN_RUNTIME);
        if (!foreignRuntime) {
            return {};
        }
        candidates = foreignRuntime->GetMemberDeclPtrs();
    } else {
        candidates = FieldLookup(ctx, Ty::GetDeclOfTy(&runtimeTy), name, {&runtimeTy, file});
    }
    Ptr<FuncDecl> func = nullptr;
    for (auto decl : candidates) {
        auto fd = DynamicCast<FuncDecl*>(decl);
        if (!fd || fd->identifier != name || !fd->TestAttr(Attribute::STATIC)) {
            continue;
        }
        // Prefer the implementation over the abstract declaration in ForeignRuntime.
        if (!func || (func->TestAttr(Attribute::ABSTRACT) && !fd->TestAttr(Attribute::ABSTRACT))) {
            func = fd;
        }
    }
    if (!func) {
        return {};
    }
    Ptr<Ty> matchedParentTy = nullptr;
    if (func->outerDecl && func->outerDecl->astKind == ASTKind::INTERFACE_DECL) {
        auto promoted = promotion.Promote(runtimeTy, *func->outerDecl->GetTy());
        if (!promoted.empty()) {
            matchedParentTy = *promoted.begin();
        }
    }
    return {func, matchedParentTy};
}

void TypeChecker::TypeCheckerImpl::DesugarExternConversions(ASTContext& ctx, Package& pkg)
{
    if (!importManager.GetCoreDecl<InterfaceDecl>(STD_LIB_FOREIGN_RUNTIME)) {
        return;
    }
    auto needConversion = [this](Ty& from, Ty& to) { return NeedExternConversion(from, to); };
    auto lookup = [this, &ctx](Ty& runtimeTy, Ptr<const File> file) {
        return LookupForeignRuntimeFunc(ctx, runtimeTy, TO_EXTERN_FUNC, file);
    };
    ExternConversion(typeManager, needConversion, lookup).Run(pkg);
}
