// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the desugaring of Extern<T>:
 * - an expression e of type U used where Extern<T> is expected is rewritten into T.toExtern<U>(e);
 * - a dynamic operation on Extern<T> values is rewritten into a direct call to the corresponding static function of
 *   T, e.g. e.f into T.memberAccess(e, "f"). Its operands are desugared on their own.
 */

#include "ExternDesugaring.h"

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

namespace {
const std::string TO_EXTERN_FUNC = "toExtern";
const std::string MEMBER_ACCESS_FUNC = "memberAccess";
const std::string INDEXED_ACCESS_FUNC = "indexedAccess";
const std::string MEMBER_UPDATE_FUNC = "memberUpdate";
const std::string INDEXED_UPDATE_FUNC = "indexedUpdate";
const std::string FUNCTION_CALL_FUNC = "functionCall";

/**
 * Create the reference RT.func to the static function @p func of the foreign runtime @p runtimeTy, typed @p funcTy.
 * @p matchedParentTy is the instantiated interface type when @p func is a member of an interface. The reference takes
 * the position of @p expr, the expression being desugared.
 */
OwnedPtr<MemberAccess> CreateForeignRuntimeFuncAccess(
    Ty& runtimeTy, FuncDecl& func, Ptr<Ty> matchedParentTy, Ty& funcTy, const Expr& expr)
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
    CopyBasicInfo(&expr, runtimeRef.get());

    auto funcAccess = CreateMemberAccess(std::move(runtimeRef), func);
    funcAccess->EnableAttr(Attribute::IMPLICIT_ADD);
    funcAccess->matchedParentTy = matchedParentTy;
    funcAccess->SetTy(&funcTy);
    return funcAccess;
}

OwnedPtr<LitConstExpr> CreateFieldName(const std::string& field, Ty& stringTy, const Expr& expr)
{
    auto name = CreateLitConstExpr(LitConstKind::STRING, field, &stringTy, true);
    CopyBasicInfo(&expr, name.get());
    return name;
}

/** The type of the values returned by @p fb. A constructor of Extern<T> has type (...) -> Extern<T> but returns no
 * value, its body is Unit. */
Ptr<Ty> GetReturnTy(const FuncBody& fb)
{
    if (!Ty::IsTyCorrect(fb.GetTy()) || !fb.GetTy()->IsFunc() ||
        (fb.funcDecl && fb.funcDecl->TestAttr(Attribute::CONSTRUCTOR))) {
        return nullptr;
    }
    return RawStaticCast<FuncTy*>(fb.GetTy())->retTy;
}

OwnedPtr<RefExpr> CreateTmpRef(VarDecl& tmp, const Expr& expr)
{
    auto ref = CreateRefExpr(tmp);
    CopyBasicInfo(&expr, ref.get());
    return ref;
}
} // namespace

void ExternDesugaring::Run(Node& root)
{
    std::function<VisitAction(Ptr<Node>)> preVisit = [this](Ptr<Node> node) -> VisitAction {
        if (auto expr = DynamicCast<Expr*>(node); expr && IsDynamic(*expr)) {
            DesugarOperation(*expr);
            // Visit the cloned operands, which may contain conversions and dynamic operations of their own.
            Run(*expr->desugarExpr);
            // The original operands are already represented in the desugared calls.
            return VisitAction::SKIP_CHILDREN;
        }
        match(*node)(
            [this](const VarDeclAbstract& vd) { HandleVarDecl(vd); },
            [this](const AssignExpr& ae) { HandleAssignExpr(ae); },
            [this](CallExpr& ce) { HandleCallExpr(ce); },
            [this](const ReturnExpr& re) { HandleReturnExpr(re); },
            [this](const FuncBody& fb) { HandleFuncBody(fb); },
            [this](ArrayExpr& ae) { HandleArrayExpr(ae); },
            [this](ArrayLit& al) { HandleArrayLit(al); },
            [this](TupleLit& tl) { HandleTupleLit(tl); },
            [this](IfExpr& ie) { HandleIfExpr(ie); },
            [this](MatchExpr& me) { HandleMatchExpr(me); },
            [this](TryExpr& te) { HandleTryExpr(te); },
            []() {});
        return VisitAction::WALK_CHILDREN;
    };
    Walker(&root, preVisit).Walk();
}

bool ExternDesugaring::IsDynamic(const Expr& expr)
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

bool ExternDesugaring::TryConvert(Expr& expr, Ty& target)
{
    auto valueTy = expr.GetTy();
    if (!valueTy || !needConversion(*valueTy, target)) {
        return false;
    }
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(expr.desugarExpr ? std::move(expr.desugarExpr) : ASTCloner::Clone(Ptr(&expr)));
    auto ce = CreateRuntimeCall(TO_EXTERN_FUNC, std::move(args), target, expr, {valueTy});
    if (!ce) {
        return false;
    }
    if (expr.astKind == ASTKind::BLOCK) {
        // Deserialization expects a block's desugarExpr to remain a Block, so wrap the conversion call.
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

void ExternDesugaring::TryConvertBlock(Block& block, Ty& target)
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

void ExternDesugaring::HandleVarDecl(const VarDeclAbstract& vd)
{
    if (vd.initializer && vd.GetTy()) {
        TryConvert(*vd.initializer, *vd.GetTy());
    }
}

void ExternDesugaring::HandleAssignExpr(const AssignExpr& ae)
{
    // Dynamic updates never get here, their value is passed as is to the update function.
    if (ae.desugarExpr || ae.isCompound || !ae.leftValue || !ae.rightExpr || !ae.leftValue->GetTy()) {
        return;
    }
    TryConvert(*ae.rightExpr, *ae.leftValue->GetTy());
}

void ExternDesugaring::HandleCallExpr(CallExpr& ce)
{
    if (!ce.baseFunc || !Ty::IsTyCorrect(ce.baseFunc->GetTy()) || !ce.baseFunc->GetTy()->IsFunc()) {
        return;
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
}

void ExternDesugaring::HandleReturnExpr(const ReturnExpr& re)
{
    if (!re.expr || !re.refFuncBody) {
        return;
    }
    if (auto retTy = GetReturnTy(*re.refFuncBody)) {
        TryConvert(re.desugarExpr ? *re.desugarExpr : *re.expr, *retTy);
    }
}

void ExternDesugaring::HandleFuncBody(const FuncBody& fb)
{
    if (!fb.body) {
        return;
    }
    if (auto retTy = GetReturnTy(fb); retTy && retTy->IsCoreExternType()) {
        TryConvertBlock(*fb.body, *retTy);
    }
}

void ExternDesugaring::HandleArrayExpr(ArrayExpr& ae)
{
    if (!Ty::IsTyCorrect(ae.GetTy()) || ae.initFunc || ae.args.empty()) {
        return;
    }
    auto typeArgs = typeManager.GetTypeArgs(*ae.GetTy());
    if (typeArgs.empty() || !typeArgs[0]) {
        return;
    }
    // VArray takes the element as its only argument, Array(size, item: T) as its second one.
    Ptr<FuncArg> arg = ae.isValueArray ? ae.args[0].get() : (ae.args.size() > 1 ? ae.args[1].get() : nullptr);
    if (arg && arg->expr && TryConvert(*arg->expr, *typeArgs[0])) {
        arg->SetTy(arg->expr->GetTy());
    }
}

void ExternDesugaring::HandleArrayLit(ArrayLit& al)
{
    auto ty = al.GetTy();
    if (al.desugarExpr || !Ty::IsTyCorrect(ty) || !ty->IsStructArray() || ty->typeArgs.empty()) {
        return;
    }
    for (auto& child : al.children) {
        TryConvert(*child, *ty->typeArgs[0]);
    }
}

void ExternDesugaring::HandleTupleLit(TupleLit& tl)
{
    auto ty = tl.GetTy();
    if (tl.desugarExpr || !Ty::IsTyCorrect(ty) || !ty->IsTuple()) {
        return;
    }
    for (size_t i = 0; i < tl.children.size() && i < ty->typeArgs.size(); ++i) {
        TryConvert(*tl.children[i], *ty->typeArgs[i]);
    }
}

void ExternDesugaring::HandleIfExpr(IfExpr& ie)
{
    auto ty = ie.GetTy();
    if (ie.desugarExpr || !Ty::IsTyCorrect(ty)) {
        return;
    }
    TryConvertBlock(*ie.thenBody, *ty);
    if (auto elseBlock = DynamicCast<Block*>(ie.elseBody.get())) {
        TryConvertBlock(*elseBlock, *ty);
    } else if (ie.elseBody) {
        TryConvert(*ie.elseBody, *ty);
    }
}

void ExternDesugaring::HandleMatchExpr(MatchExpr& me)
{
    auto ty = me.GetTy();
    if (me.desugarExpr || !Ty::IsTyCorrect(ty)) {
        return;
    }
    for (auto& mc : me.matchCases) {
        TryConvertBlock(*mc->exprOrDecls, *ty);
        mc->SetTy(mc->exprOrDecls->GetTy());
    }
    for (auto& mco : me.matchCaseOthers) {
        TryConvertBlock(*mco->exprOrDecls, *ty);
        mco->SetTy(mco->exprOrDecls->GetTy());
    }
}

void ExternDesugaring::HandleTryExpr(TryExpr& te)
{
    auto ty = te.GetTy();
    if (te.desugarExpr || !Ty::IsTyCorrect(ty)) {
        return;
    }
    TryConvertBlock(*te.tryBlock, *ty);
    for (auto& catchBlock : te.catchBlocks) {
        TryConvertBlock(*catchBlock, *ty);
    }
}

void ExternDesugaring::DesugarOperation(Expr& expr)
{
    auto desugared = match(expr)(
        [this](MemberAccess& ma) { return BuildMemberAccess(ma, *ma.baseExpr->GetTy()); },
        [this](SubscriptExpr& se) { return BuildIndexedAccess(se, *se.baseExpr->GetTy()); },
        [this](CallExpr& ce) { return BuildFunctionCall(ce, *ce.baseFunc->GetTy()); },
        [this](AssignExpr& ae) {
            // The left value of a dynamic update has the type of its receiver.
            auto& externTy = *ae.leftValue->GetTy();
            return ae.isCompound ? BuildCompoundAssignment(ae, externTy) : BuildUpdate(ae, externTy);
        },
        []() -> OwnedPtr<Expr> { return nullptr; });
    CJC_NULLPTR_CHECK(desugared); // for well typed programs the functions of the runtime exist
    expr.desugarExpr = std::move(desugared);
    AddCurFile(*expr.desugarExpr, expr.curFile);
}

OwnedPtr<Expr> ExternDesugaring::BuildMemberAccess(MemberAccess& ma, Ty& externTy)
{
    return CreateMemberAccessCall(ASTCloner::Clone(ma.baseExpr.get()), ma.field.Val(), externTy, ma);
}

OwnedPtr<Expr> ExternDesugaring::BuildIndexedReceiver(SubscriptExpr& se, Ty& externTy)
{
    // e[i1, ..., in] is desugared like e[i1]...[in].
    OwnedPtr<Expr> receiver = ASTCloner::Clone(se.baseExpr.get());
    for (size_t i = 0; i + 1 < se.indexExprs.size(); ++i) {
        std::vector<OwnedPtr<Expr>> args;
        args.emplace_back(std::move(receiver));
        args.emplace_back(ASTCloner::Clone(se.indexExprs[i].get()));
        receiver = CreateRuntimeCall(INDEXED_ACCESS_FUNC, std::move(args), externTy, se);
    }
    return receiver;
}

OwnedPtr<Expr> ExternDesugaring::BuildIndexedAccess(SubscriptExpr& se, Ty& externTy)
{
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(BuildIndexedReceiver(se, externTy));
    args.emplace_back(ASTCloner::Clone(se.indexExprs.back().get()));
    return CreateRuntimeCall(INDEXED_ACCESS_FUNC, std::move(args), externTy, se);
}

OwnedPtr<Expr> ExternDesugaring::BuildFunctionCall(CallExpr& ce, Ty& externTy)
{
    std::vector<OwnedPtr<Expr>> args;
    for (auto& arg : ce.args) {
        args.emplace_back(ASTCloner::Clone(arg->expr.get()));
    }
    return CreateFunctionCall(ASTCloner::Clone(ce.baseFunc.get()), std::move(args), externTy, ce);
}

OwnedPtr<Expr> ExternDesugaring::BuildUpdate(AssignExpr& ae, Ty& externTy)
{
    std::vector<OwnedPtr<Expr>> args;
    if (auto ma = DynamicCast<MemberAccess*>(ae.leftValue.get())) {
        auto func = LookupRuntimeFunc(MEMBER_UPDATE_FUNC, externTy, {}, ae);
        if (!func.decl) {
            return nullptr;
        }
        args.emplace_back(ASTCloner::Clone(ma->baseExpr.get()));
        args.emplace_back(CreateFieldName(ma->field.Val(), *func.ty->paramTys[1], *ma));
        args.emplace_back(ASTCloner::Clone(ae.rightExpr.get()));
        return CreateRuntimeCall(func, std::move(args), externTy, ae);
    }
    auto& se = StaticCast<SubscriptExpr&>(*ae.leftValue);
    args.emplace_back(BuildIndexedReceiver(se, externTy));
    args.emplace_back(ASTCloner::Clone(se.indexExprs.back().get()));
    args.emplace_back(ASTCloner::Clone(ae.rightExpr.get()));
    return CreateRuntimeCall(INDEXED_UPDATE_FUNC, std::move(args), externTy, ae);
}

/**
 * e.f op= v and e[i] op= v are desugared into a block evaluating the receiver and the index only once:
 *     let r = e; let o = T.memberAccess(T.memberAccess(r, "f"), "op"); T.memberUpdate(r, "f", T.functionCall(o, [v]))
 *     let r = e; let i = i; let o = T.memberAccess(T.indexedAccess(r, i), "op");
 *     T.indexedUpdate(r, i, T.functionCall(o, [v]))
 */
OwnedPtr<Expr> ExternDesugaring::BuildCompoundAssignment(AssignExpr& ae, Ty& externTy)
{
    const std::string op = TOKENS[static_cast<int>(COMPOUND_ASSIGN_EXPR_MAP.at(ae.op))];
    std::vector<OwnedPtr<Node>> nodes;
    auto addTmp = [&nodes, &ae](const std::string& name, OwnedPtr<Expr> init) -> Ptr<VarDecl> {
        if (!init) {
            return nullptr;
        }
        auto tmp = CreateVarDecl(name, std::move(init));
        CopyBasicInfo(&ae, tmp.get());
        auto ptr = tmp.get();
        nodes.emplace_back(std::move(tmp));
        return ptr;
    };
    auto applyOp = [this, &ae, &externTy](VarDecl& opFunc) {
        std::vector<OwnedPtr<Expr>> args;
        args.emplace_back(ASTCloner::Clone(ae.rightExpr.get()));
        return CreateFunctionCall(CreateTmpRef(opFunc, ae), std::move(args), externTy, ae);
    };
    OwnedPtr<Expr> update;
    if (auto ma = DynamicCast<MemberAccess*>(ae.leftValue.get())) {
        auto receiver = addTmp("$externReceiver", ASTCloner::Clone(ma->baseExpr.get()));
        auto func = LookupRuntimeFunc(MEMBER_UPDATE_FUNC, externTy, {}, ae);
        if (!receiver || !func.decl) {
            return nullptr;
        }
        auto current = CreateMemberAccessCall(CreateTmpRef(*receiver, ae), ma->field.Val(), externTy, *ma);
        auto opFunc = addTmp("$externOp", CreateMemberAccessCall(std::move(current), op, externTy, ae));
        if (!opFunc) {
            return nullptr;
        }
        std::vector<OwnedPtr<Expr>> args;
        args.emplace_back(CreateTmpRef(*receiver, ae));
        args.emplace_back(CreateFieldName(ma->field.Val(), *func.ty->paramTys[1], *ma));
        args.emplace_back(applyOp(*opFunc));
        update = CreateRuntimeCall(func, std::move(args), externTy, ae);
    } else {
        auto& se = StaticCast<SubscriptExpr&>(*ae.leftValue);
        auto receiver = addTmp("$externReceiver", BuildIndexedReceiver(se, externTy));
        auto index = addTmp("$externIndex", ASTCloner::Clone(se.indexExprs.back().get()));
        if (!receiver || !index) {
            return nullptr;
        }
        std::vector<OwnedPtr<Expr>> accessArgs;
        accessArgs.emplace_back(CreateTmpRef(*receiver, ae));
        accessArgs.emplace_back(CreateTmpRef(*index, ae));
        auto current = CreateRuntimeCall(INDEXED_ACCESS_FUNC, std::move(accessArgs), externTy, se);
        auto opFunc = addTmp("$externOp", CreateMemberAccessCall(std::move(current), op, externTy, ae));
        if (!opFunc) {
            return nullptr;
        }
        std::vector<OwnedPtr<Expr>> args;
        args.emplace_back(CreateTmpRef(*receiver, ae));
        args.emplace_back(CreateTmpRef(*index, ae));
        args.emplace_back(applyOp(*opFunc));
        update = CreateRuntimeCall(INDEXED_UPDATE_FUNC, std::move(args), externTy, ae);
    }
    if (!update) {
        return nullptr;
    }
    nodes.emplace_back(std::move(update));
    auto block = CreateBlock(std::move(nodes), TypeManager::GetPrimitiveTy(TypeKind::TYPE_UNIT));
    CopyBasicInfo(&ae, block.get());
    return block;
}

OwnedPtr<Expr> ExternDesugaring::CreateMemberAccessCall(
    OwnedPtr<Expr> receiver, const std::string& field, Ty& externTy, const Expr& expr)
{
    auto func = LookupRuntimeFunc(MEMBER_ACCESS_FUNC, externTy, {}, expr);
    if (!func.decl) {
        return nullptr;
    }
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(std::move(receiver));
    args.emplace_back(CreateFieldName(field, *func.ty->paramTys[1], expr));
    return CreateRuntimeCall(func, std::move(args), externTy, expr);
}

OwnedPtr<Expr> ExternDesugaring::CreateFunctionCall(
    OwnedPtr<Expr> callee, std::vector<OwnedPtr<Expr>> args, Ty& externTy, const Expr& expr)
{
    auto func = LookupRuntimeFunc(FUNCTION_CALL_FUNC, externTy, {}, expr);
    if (!func.decl || std::any_of(args.begin(), args.end(), [](auto& arg) { return !arg; })) {
        return nullptr;
    }
    auto arrayLit = CreateArrayLit(std::move(args), func.ty->paramTys[1]);
    AddArrayLitConstructor(*arrayLit);
    if (!arrayLit->initFunc) {
        return nullptr;
    }
    CopyBasicInfo(&expr, arrayLit.get());
    arrayLit->EnableAttr(Attribute::IMPLICIT_ADD);
    std::vector<OwnedPtr<Expr>> callArgs;
    callArgs.emplace_back(std::move(callee));
    callArgs.emplace_back(std::move(arrayLit));
    return CreateRuntimeCall(func, std::move(callArgs), externTy, expr);
}

/**
 * Look up the static function @p name of the foreign runtime T of @p externTy. Its type is instantiated with
 * @p externTy for Extern<T>, and with the only element of @p instTys for its own type parameter, if any.
 */
ExternDesugaring::RuntimeFunc ExternDesugaring::LookupRuntimeFunc(
    const std::string& name, Ty& externTy, const std::vector<Ptr<Ty>>& instTys, const Expr& expr)
{
    CJC_ASSERT(externTy.IsCoreExternType());
    auto [decl, matchedParentTy] = lookup(*externTy.typeArgs[0], name, expr);
    auto declTy = decl ? DynamicCast<FuncTy*>(decl->GetTy()) : nullptr;
    if (!Ty::IsTyCorrect(declTy)) {
        return {};
    }
    auto instantiate = [&externTy, &instTys](Ptr<Ty> ty) -> Ptr<Ty> {
        if (ty->IsCoreExternType()) {
            return &externTy;
        }
        return ty->IsGeneric() && !instTys.empty() ? instTys[0] : ty;
    };
    std::vector<Ptr<Ty>> paramTys;
    for (auto paramTy : declTy->paramTys) {
        paramTys.emplace_back(instantiate(paramTy));
    }
    return {decl, matchedParentTy, typeManager.GetFunctionTy(paramTys, instantiate(declTy->retTy))};
}

/** Create the call T.func<instTys>(args) for the desugaring of @p expr, where T is the foreign runtime of @p externTy. */
OwnedPtr<CallExpr> ExternDesugaring::CreateRuntimeCall(const RuntimeFunc& func, std::vector<OwnedPtr<Expr>> args,
    Ty& externTy, const Expr& expr, std::vector<Ptr<Ty>> instTys)
{
    CJC_ASSERT(func.decl && func.ty && func.ty->paramTys.size() == args.size());
    if (std::any_of(args.begin(), args.end(), [](auto& arg) { return !arg; })) {
        return nullptr;
    }
    auto baseFunc =
        CreateForeignRuntimeFuncAccess(*externTy.typeArgs[0], *func.decl, func.matchedParentTy, *func.ty, expr);
    if (!baseFunc) {
        return nullptr;
    }
    baseFunc->instTys = std::move(instTys);
    std::vector<OwnedPtr<FuncArg>> funcArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        // Operands keep their own type, even where the parameter is Any.
        auto argTy = Ty::IsTyCorrect(args[i]->GetTy()) ? args[i]->GetTy() : func.ty->paramTys[i];
        auto arg = CreateFuncArg(std::move(args[i]), "", argTy);
        CopyBasicInfo(&expr, arg.get());
        funcArgs.emplace_back(std::move(arg));
    }
    auto ce = CreateCallExpr(
        std::move(baseFunc), std::move(funcArgs), func.decl, func.ty->retTy, CallKind::CALL_DECLARED_FUNCTION);
    ce->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&expr, ce.get());
    return ce;
}

OwnedPtr<CallExpr> ExternDesugaring::CreateRuntimeCall(const std::string& name, std::vector<OwnedPtr<Expr>> args,
    Ty& externTy, const Expr& expr, std::vector<Ptr<Ty>> instTys)
{
    auto func = LookupRuntimeFunc(name, externTy, instTys, expr);
    if (!func.decl) {
        return nullptr;
    }
    return CreateRuntimeCall(func, std::move(args), externTy, expr, std::move(instTys));
}

ForeignRuntimeFunc TypeChecker::TypeCheckerImpl::LookupForeignRuntimeFunc(
    ASTContext& ctx, Ty& runtimeTy, const std::string& name, const Expr& expr)
{
    std::vector<Ptr<Decl>> candidates;
    if (runtimeTy.IsGeneric()) {
        auto foreignRuntime = importManager.GetCoreDecl<InterfaceDecl>(STD_LIB_FOREIGN_RUNTIME);
        if (!foreignRuntime) {
            CJC_ASSERT(false);
            return {};
        }
        candidates = foreignRuntime->GetMemberDeclPtrs();
    } else {
        candidates = FieldLookup(ctx, Ty::GetDeclOfTy(&runtimeTy), name, {&runtimeTy, expr.curFile});
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
    // Like a static call T.name(...) written by hand, the call must have an implementation.
    if (!runtimeTy.IsGeneric() && func->TestAttr(Attribute::ABSTRACT)) {
        diag.DiagnoseRefactor(DiagKindRefactor::sema_interface_call_with_unimplemented_call, expr, "function", name);
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

void TypeChecker::TypeCheckerImpl::DesugarExtern(ASTContext& ctx, Package& pkg)
{
    auto needConversion = [this](Ty& from, Ty& to) { return NeedExternConversion(from, to); };
    auto lookup = [this, &ctx](Ty& runtimeTy, const std::string& name, const Expr& expr) {
        return LookupForeignRuntimeFunc(ctx, runtimeTy, name, expr);
    };
    auto externDesugaring = ExternDesugaring(typeManager, needConversion, lookup);
    externDesugaring.Run(pkg);
}
