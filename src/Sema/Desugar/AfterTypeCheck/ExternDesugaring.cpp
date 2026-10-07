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
 * - the outermost dynamic operation on Extern<T> values is rewritten into T.eval(tree), where tree is built from the
 *   primitive constructors of Extern<T>. Nested dynamic operations become nodes of the tree, any other expression is
 *   kept as a leaf and desugared on its own.
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
const std::string EVAL_FUNC = "eval";
const std::string EXTERN_MEMBER_ACCESS_CTOR = "ExternMemberAccess";
const std::string EXTERN_INDEXED_ACCESS_CTOR = "ExternIndexedAccess";
const std::string EXTERN_MEMBER_UPDATE_CTOR = "ExternMemberUpdate";
const std::string EXTERN_INDEXED_UPDATE_CTOR = "ExternIndexedUpdate";
const std::string EXTERN_FUNCTION_CALL_CTOR = "ExternFunctionCall";
const std::string EXTERN_COMPOUND_ASSIGNMENT_CTOR = "ExternCompoundAssignment";

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
} // namespace

void ExternDesugaring::Run(Node& root)
{
    std::function<VisitAction(Ptr<Node>)> preVisit = [this](Ptr<Node> node) -> VisitAction {
        if (auto expr = DynamicCast<Expr*>(node); expr && IsDynamic(*expr)) {
            DesugarOperation(*expr);
            // Visit the cloned leaves, which may contain conversions and dynamic operations of their own.
            Run(*expr->desugarExpr);
            // The original operands are already represented in the tree.
            return VisitAction::SKIP_CHILDREN;
        }
        match(*node)(
            [this](const VarDecl& vd) { HandleVarDecl(vd); },
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
    auto value = expr.desugarExpr ? std::move(expr.desugarExpr) : ASTCloner::Clone(Ptr(&expr));
    auto ce = CreateRuntimeCall(TO_EXTERN_FUNC, std::move(value), *valueTy, target, {valueTy}, expr);
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

void ExternDesugaring::HandleVarDecl(const VarDecl& vd)
{
    if (vd.initializer && vd.GetTy()) {
        TryConvert(*vd.initializer, *vd.GetTy());
    }
}

void ExternDesugaring::HandleAssignExpr(const AssignExpr& ae)
{
    // Dynamic updates never get here, their value is kept as is in the tree.
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
    if (!re.expr || !re.refFuncBody || !Ty::IsTyCorrect(re.refFuncBody->GetTy()) ||
        !re.refFuncBody->GetTy()->IsFunc()) {
        return;
    }
    auto retTy = RawStaticCast<FuncTy*>(re.refFuncBody->GetTy())->retTy;
    if (retTy) {
        TryConvert(re.desugarExpr ? *re.desugarExpr : *re.expr, *retTy);
    }
}

void ExternDesugaring::HandleFuncBody(const FuncBody& fb)
{
    if (!fb.body || !Ty::IsTyCorrect(fb.GetTy()) || !fb.GetTy()->IsFunc()) {
        return;
    }
    auto retTy = RawStaticCast<FuncTy*>(fb.GetTy())->retTy;
    if (retTy && retTy->IsCoreExternType()) {
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
    auto& externTy = *expr.GetTy();
    auto eval = CreateRuntimeCall(EVAL_FUNC, BuildTree(expr), externTy, externTy, {}, expr);
    CJC_NULLPTR_CHECK(eval); // for well typed programs eval != nullptr
    expr.desugarExpr = std::move(eval);
    AddCurFile(*expr.desugarExpr, expr.curFile);
}

OwnedPtr<Expr> ExternDesugaring::BuildTree(Expr& expr)
{
    if (auto pe = DynamicCast<ParenExpr*>(&expr); pe && pe->expr && IsDynamic(*pe->expr)) {
        return BuildTree(*pe->expr);
    }
    if (!IsDynamic(expr)) {
        return ASTCloner::Clone(Ptr(&expr));
    }
    return BuildOperation(expr, *expr.GetTy());
}

OwnedPtr<Expr> ExternDesugaring::BuildOperation(Expr& expr, Ty& externTy)
{
    return match(expr)(
        [this, &externTy](MemberAccess& ma) { return BuildMemberOperation(ma, externTy, ma); },
        [this, &externTy](SubscriptExpr& se) { return BuildIndexedOperation(se, externTy, se); },
        [this, &externTy](CallExpr& ce) { return BuildCallOperation(ce, externTy); },
        [this, &externTy](AssignExpr& ae) { return BuildAssignmentOperation(ae, externTy); },
        []() -> OwnedPtr<Expr> {
            CJC_ASSERT(false);
            return nullptr;
        });
}

OwnedPtr<Expr> ExternDesugaring::BuildMemberOperation(
    MemberAccess& ma, Ty& externTy, const Expr& source, Ptr<Expr> value)
{
    auto ctor = LookupCtor(value ? EXTERN_MEMBER_UPDATE_CTOR : EXTERN_MEMBER_ACCESS_CTOR, externTy);
    if (!ctor.decl) {
        return nullptr;
    }
    auto field = CreateLitConstExpr(LitConstKind::STRING, ma.field.Val(), ctor.ty->paramTys[1], true);
    CopyBasicInfo(&ma, field.get());
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(BuildTree(*ma.baseExpr));
    args.emplace_back(std::move(field));
    if (value) {
        args.emplace_back(BuildTree(*value));
    }
    if (std::any_of(args.begin(), args.end(), [](auto& arg) { return !arg; })) {
        return nullptr;
    }
    return CreateCtorCall(ctor, externTy, std::move(args), source);
}

OwnedPtr<Expr> ExternDesugaring::BuildIndexedOperation(
    SubscriptExpr& se, Ty& externTy, const Expr& source, Ptr<Expr> value)
{
    // e[i1, ..., in] is built like e[i1]...[in]; an update changes only the final access.
    auto receiver = BuildTree(*se.baseExpr);
    auto accessCtor = LookupCtor(EXTERN_INDEXED_ACCESS_CTOR, externTy);
    for (size_t i = 0; i + 1 < se.indexExprs.size(); ++i) {
        auto index = BuildTree(*se.indexExprs[i]);
        if (!accessCtor.decl || !receiver || !index) {
            return nullptr;
        }
        std::vector<OwnedPtr<Expr>> accessArgs;
        accessArgs.emplace_back(std::move(receiver));
        accessArgs.emplace_back(std::move(index));
        receiver = CreateCtorCall(accessCtor, externTy, std::move(accessArgs), se);
    }
    auto ctor = value ? LookupCtor(EXTERN_INDEXED_UPDATE_CTOR, externTy) : accessCtor;
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(std::move(receiver));
    args.emplace_back(BuildTree(*se.indexExprs.back()));
    if (value) {
        args.emplace_back(BuildTree(*value));
    }
    if (!ctor.decl || std::any_of(args.begin(), args.end(), [](auto& arg) { return !arg; })) {
        return nullptr;
    }
    return CreateCtorCall(ctor, externTy, std::move(args), source);
}

OwnedPtr<Expr> ExternDesugaring::BuildCallOperation(CallExpr& ce, Ty& externTy)
{
    auto ctor = LookupCtor(EXTERN_FUNCTION_CALL_CTOR, externTy);
    if (!ctor.decl) {
        return nullptr;
    }
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(BuildTree(*ce.baseFunc));
    args.emplace_back(BuildArgs(ce, *ctor.ty->paramTys[1]));
    if (std::any_of(args.begin(), args.end(), [](auto& arg) { return !arg; })) {
        return nullptr;
    }
    return CreateCtorCall(ctor, externTy, std::move(args), ce);
}

OwnedPtr<Expr> ExternDesugaring::BuildAssignmentOperation(AssignExpr& ae, Ty& externTy)
{
    if (ae.isCompound) {
        return BuildCompoundAssignment(ae, externTy);
    }
    // An update uses the access to its left value, with the assigned value as an extra argument.
    return match(*ae.leftValue)(
        [this, &ae, &externTy](MemberAccess& ma) {
            return BuildMemberOperation(ma, externTy, ae, ae.rightExpr.get());
        },
        [this, &ae, &externTy](SubscriptExpr& se) {
            return BuildIndexedOperation(se, externTy, ae, ae.rightExpr.get());
        },
        []() -> OwnedPtr<Expr> {
            CJC_ASSERT(false);
            return nullptr;
        });
}

OwnedPtr<Expr> ExternDesugaring::BuildCompoundAssignment(AssignExpr& ae, Ty& externTy)
{
    auto ctor = LookupCtor(EXTERN_COMPOUND_ASSIGNMENT_CTOR, externTy);
    if (!ctor.decl) {
        return nullptr;
    }
    // e.f op= v is built from the access e.f and the binary operator op, without '='.
    // BuildOperation also handles left values; BuildTree would treat the target as a non-dynamic leaf.
    auto target = BuildOperation(*ae.leftValue, externTy);
    auto op = CreateLitConstExpr(LitConstKind::STRING,
        TOKENS[static_cast<int>(COMPOUND_ASSIGN_EXPR_MAP.at(ae.op))], ctor.ty->paramTys[1], true);
    CopyBasicInfo(&ae, op.get());
    auto value = BuildTree(*ae.rightExpr);
    if (!target || !value) {
        return nullptr;
    }
    std::vector<OwnedPtr<Expr>> args;
    args.emplace_back(std::move(target));
    args.emplace_back(std::move(op));
    args.emplace_back(std::move(value));
    return CreateCtorCall(ctor, externTy, std::move(args), ae);
}

OwnedPtr<Expr> ExternDesugaring::BuildArgs(CallExpr& ce, Ty& arrayTy)
{
    std::vector<OwnedPtr<Expr>> elements;
    for (auto& arg : ce.args) {
        auto element = BuildTree(*arg->expr);
        if (!element) {
            return nullptr;
        }
        elements.emplace_back(std::move(element));
    }
    auto arrayLit = CreateArrayLit(std::move(elements), &arrayTy);
    AddArrayLitConstructor(*arrayLit);
    if (!arrayLit->initFunc) {
        return nullptr;
    }
    CopyBasicInfo(&ce, arrayLit.get());
    arrayLit->EnableAttr(Attribute::IMPLICIT_ADD);
    return arrayLit;
}

ExternDesugaring::Ctor ExternDesugaring::LookupCtor(const std::string& name, Ty& externTy)
{
    auto externDecl = Ty::GetDeclOfTy(&externTy);
    auto ctor = DynamicCast<FuncDecl*>(Sema::Desugar::AfterTypeCheck::LookupEnumMember(externDecl, name));
    if (!ctor || !Ty::IsTyCorrect(ctor->GetTy())) {
        return {};
    }
    auto ctorTy = DynamicCast<FuncTy*>(
        typeManager.GetInstantiatedTy(ctor->GetTy(), GenerateTypeMapping(*externDecl, externTy.typeArgs)));
    return ctorTy ? Ctor{ctor, ctorTy} : Ctor{};
}

/** Create the call of the constructor @p ctor, for the desugaring of @p expr. */
OwnedPtr<CallExpr> ExternDesugaring::CreateCtorCall(
    const Ctor& ctor, Ty& externTy, std::vector<OwnedPtr<Expr>> args, const Expr& expr)
{
    CJC_ASSERT(ctor.decl && ctor.ty && ctor.ty->paramTys.size() == args.size());
    auto ctorRef = CreateRefExpr(*ctor.decl);
    ctorRef->SetTy(ctor.ty);
    ctorRef->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&expr, ctorRef.get());
    std::vector<OwnedPtr<FuncArg>> funcArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        auto argTy = args[i]->GetTy();
        auto arg = CreateFuncArg(std::move(args[i]), "", argTy);
        CopyBasicInfo(&expr, arg.get());
        funcArgs.emplace_back(std::move(arg));
    }
    auto ce = CreateCallExpr(
        std::move(ctorRef), std::move(funcArgs), ctor.decl, &externTy, CallKind::CALL_DECLARED_FUNCTION);
    ce->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&expr, ce.get());
    return ce;
}

/**
 * Create the call T.name<instTys>(arg) to the static function @p name of the foreign runtime T of @p externTy, which
 * takes @p argTy and returns @p externTy, for the desugaring of @p expr.
 */
OwnedPtr<CallExpr> ExternDesugaring::CreateRuntimeCall(const std::string& name, OwnedPtr<Expr> arg, Ty& argTy,
    Ty& externTy, std::vector<Ptr<Ty>> instTys, const Expr& expr)
{
    CJC_ASSERT(externTy.IsCoreExternType());
    auto runtimeTy = externTy.typeArgs[0];
    auto [func, matchedParentTy] = lookup(*runtimeTy, name, expr);
    if (!func) {
        return nullptr;
    }
    auto baseFunc = CreateForeignRuntimeFuncAccess(
        *runtimeTy, *func, matchedParentTy, *typeManager.GetFunctionTy({&argTy}, &externTy), expr);
    if (!baseFunc) {
        return nullptr;
    }
    baseFunc->instTys = std::move(instTys);
    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateFuncArg(std::move(arg), "", &argTy));
    auto ce = CreateCallExpr(std::move(baseFunc), std::move(args), func, &externTy, CallKind::CALL_DECLARED_FUNCTION);
    ce->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&expr, ce.get());
    return ce;
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
