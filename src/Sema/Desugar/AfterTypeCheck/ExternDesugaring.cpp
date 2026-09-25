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
 * @p matchedParentTy is the instantiated interface type when @p func is a member of an interface.
 */
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

class ExternDesugaring {
public:
    using NeedConversion = std::function<bool(Ty& from, Ty& to)>;
    using RuntimeFuncLookup =
        std::function<ForeignRuntimeFunc(Ty& runtimeTy, const std::string& name, const Expr& pos)>;

    ExternDesugaring(TypeManager& typeManager, NeedConversion needConversion, RuntimeFuncLookup lookup)
        : typeManager(typeManager), needConversion(std::move(needConversion)), lookup(std::move(lookup))
    {
    }

    void Run(Node& root)
    {
        std::function<VisitAction(Ptr<Node>)> preVisit = [this](Ptr<Node> node) -> VisitAction {
            if (auto ae = DynamicCast<AssignExpr*>(node)) {
                PrepareStaticCompoundAssignment(*ae);
            }
            if (auto expr = DynamicCast<Expr*>(node); expr && IsDynamic(*expr)) {
                DesugarOperation(*expr);
                return VisitAction::SKIP_CHILDREN;
            }
            return match(*node)([this](const VarDecl& vd) { return HandleVarDecl(vd); },
                [this](const AssignExpr& ae) { return HandleAssignExpr(ae); },
                [this](CallExpr& ce) { return HandleCallExpr(ce); },
                [this](const ReturnExpr& re) { return HandleReturnExpr(re); },
                [this](const FuncBody& fb) { return HandleFuncBody(fb); },
                [this](ArrayExpr& ae) { return HandleArrayExpr(ae); },
                []() { return VisitAction::WALK_CHILDREN; });
        };
        Walker(&root, preVisit).Walk();
    }

private:
    static bool IsDynamic(const Expr& expr)
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

    /** A primitive constructor of Extern<T>, with its type instantiated for a given T. */
    struct Ctor {
        Ptr<FuncDecl> decl{nullptr};
        Ptr<FuncTy> ty{nullptr};
    };

    /** Whether evaluating @p expr more than once has no observable effect. */
    static bool IsSideEffectFree(const Expr& expr)
    {
        auto target = expr.GetTarget();
        bool isPureTarget = target &&
            (target->IsTypeDecl() || target->astKind == ASTKind::PACKAGE_DECL ||
                (Is<VarDecl>(target) && !Is<PropDecl>(target)));
        if (expr.astKind == ASTKind::REF_EXPR) {
            return isPureTarget;
        }
        auto ma = DynamicCast<const MemberAccess*>(&expr);
        return ma && isPureTarget && ma->baseExpr && IsSideEffectFree(*ma->baseExpr);
    }

    // Conversions to Extern<T>.
    bool TryConvert(Expr& expr, Ty& target);
    void TryConvertBlock(Block& block, Ty& target);

    VisitAction HandleVarDecl(const VarDecl& vd);
    VisitAction HandleAssignExpr(const AssignExpr& ae);
    VisitAction HandleCallExpr(CallExpr& ce);
    VisitAction HandleReturnExpr(const ReturnExpr& re);
    VisitAction HandleFuncBody(const FuncBody& fb);
    VisitAction HandleArrayExpr(ArrayExpr& ae);

    // Dynamic operations on Extern<T>.
    void PrepareStaticCompoundAssignment(AssignExpr& ae);
    void DesugarOperation(Expr& expr);
    OwnedPtr<Expr> BuildTree(Expr& expr);
    OwnedPtr<Expr> BuildOperation(Expr& expr, Ty& externTy);
    OwnedPtr<Expr> BuildCompoundAssignment(AssignExpr& ae, Ty& externTy);
    OwnedPtr<Expr> BuildArgs(CallExpr& ce, Ty& arrayTy);
    Ctor LookupCtor(const std::string& name, Ty& externTy);
    static OwnedPtr<CallExpr> CreateCtorCall(
        const Ctor& ctor, Ty& externTy, std::vector<OwnedPtr<Expr>> args, const Node& pos);

    OwnedPtr<CallExpr> CreateRuntimeCall(const std::string& name, OwnedPtr<Expr> arg, Ty& argTy, Ty& externTy,
        std::vector<Ptr<Ty>> instTys, const Expr& pos);

    TypeManager& typeManager;
    NeedConversion needConversion;
    RuntimeFuncLookup lookup;
};

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

VisitAction ExternDesugaring::HandleVarDecl(const VarDecl& vd)
{
    if (vd.initializer && vd.GetTy()) {
        TryConvert(*vd.initializer, *vd.GetTy());
    }
    return VisitAction::WALK_CHILDREN;
}

VisitAction ExternDesugaring::HandleAssignExpr(const AssignExpr& ae)
{
    // Dynamic updates never get here, their value is kept as is in the tree.
    if (ae.desugarExpr || ae.isCompound || !ae.leftValue || !ae.rightExpr || !ae.leftValue->GetTy()) {
        return VisitAction::WALK_CHILDREN;
    }
    TryConvert(*ae.rightExpr, *ae.leftValue->GetTy());
    return VisitAction::WALK_CHILDREN;
}

VisitAction ExternDesugaring::HandleCallExpr(CallExpr& ce)
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

VisitAction ExternDesugaring::HandleReturnExpr(const ReturnExpr& re)
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

VisitAction ExternDesugaring::HandleFuncBody(const FuncBody& fb)
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

VisitAction ExternDesugaring::HandleArrayExpr(ArrayExpr& ae)
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

/**
 * A compound assignment lhs op= v whose statically resolved left value lhs has type Extern<T> is type checked as
 * lhs = lhs'.op(v), where the copy lhs' is mapped to lhs so that the receiver of lhs is evaluated once. lhs' becomes
 * a leaf of the tree of the dynamic call and is read as a value, while the mapping yields a reference to lhs. So lhs'
 * is either evaluated again, if that has no effect, or the receiver of lhs is stored in a variable:
 * {
 *     let tmp = base
 *     tmp.x = tmp.x.op(v)
 * }
 */
void ExternDesugaring::PrepareStaticCompoundAssignment(AssignExpr& ae)
{
    auto inner = ae.isCompound ? DynamicCast<AssignExpr*>(ae.desugarExpr.get()) : nullptr;
    auto ce = inner ? DynamicCast<CallExpr*>(inner->rightExpr.get()) : nullptr;
    auto callee = ce && IsDynamic(*ce) ? DynamicCast<MemberAccess*>(ce->baseFunc.get()) : nullptr;
    if (!callee || !callee->baseExpr || callee->baseExpr->mapExpr != inner->leftValue.get()) {
        return;
    }
    auto& copy = *callee->baseExpr;
    copy.mapExpr = nullptr;
    auto lhs = DynamicCast<MemberAccess*>(inner->leftValue.get());
    if (!lhs || !lhs->baseExpr || IsSideEffectFree(*lhs->baseExpr)) {
        return;
    }
    auto vd = CreateVarDecl(V_COMPILER, std::move(lhs->baseExpr));
    vd->fullPackageName = ae.GetFullPackageName();
    CopyBasicInfo(vd->initializer.get(), vd.get());
    lhs->baseExpr = CreateRefExpr(*vd, *vd->initializer);
    CopyBasicInfo(vd->initializer.get(), lhs->baseExpr.get());
    auto& copyAccess = StaticCast<MemberAccess&>(copy);
    copyAccess.baseExpr = CreateRefExpr(*vd, *vd->initializer);
    CopyBasicInfo(vd->initializer.get(), copyAccess.baseExpr.get());
    std::vector<OwnedPtr<Node>> nodes;
    nodes.emplace_back(std::move(vd));
    nodes.emplace_back(std::move(ae.desugarExpr));
    auto block = CreateBlock(std::move(nodes), ae.GetTy());
    CopyBasicInfo(&ae, block.get());
    AddCurFile(*block, ae.curFile);
    ae.desugarExpr = std::move(block);
}

void ExternDesugaring::DesugarOperation(Expr& expr)
{
    auto& externTy = *expr.GetTy();
    auto tree = BuildTree(expr);
    auto eval = tree ? CreateRuntimeCall(EVAL_FUNC, std::move(tree), externTy, externTy, {}, expr) : nullptr;
    CJC_NULLPTR_CHECK(eval);
    expr.desugarExpr = std::move(eval);
    AddCurFile(*expr.desugarExpr, expr.curFile);
    // Leaves of the tree may contain conversions and dynamic operations of their own.
    Run(*expr.desugarExpr);
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
    // An update is built like the access to its left value, with the assigned value as an extra argument.
    auto ae = DynamicCast<AssignExpr*>(&expr);
    if (ae && ae->isCompound) {
        return BuildCompoundAssignment(*ae, externTy);
    }
    auto& access = ae ? *ae->leftValue : expr;
    Ctor ctor;
    std::vector<OwnedPtr<Expr>> args;
    if (auto ma = DynamicCast<MemberAccess*>(&access)) {
        ctor = LookupCtor(ae ? EXTERN_MEMBER_UPDATE_CTOR : EXTERN_MEMBER_ACCESS_CTOR, externTy);
        if (!ctor.decl) {
            return nullptr;
        }
        auto field = CreateLitConstExpr(LitConstKind::STRING, ma->field.Val(), ctor.ty->paramTys[1], true);
        CopyBasicInfo(ma, field.get());
        args.emplace_back(BuildTree(*ma->baseExpr));
        args.emplace_back(std::move(field));
    } else if (auto se = DynamicCast<SubscriptExpr*>(&access)) {
        // e[i1, ..., in] is built like e[i1]...[in].
        auto receiver = BuildTree(*se->baseExpr);
        auto accessCtor = LookupCtor(EXTERN_INDEXED_ACCESS_CTOR, externTy);
        for (size_t i = 0; i + 1 < se->indexExprs.size(); ++i) {
            auto index = BuildTree(*se->indexExprs[i]);
            if (!accessCtor.decl || !receiver || !index) {
                return nullptr;
            }
            std::vector<OwnedPtr<Expr>> accessArgs;
            accessArgs.emplace_back(std::move(receiver));
            accessArgs.emplace_back(std::move(index));
            receiver = CreateCtorCall(accessCtor, externTy, std::move(accessArgs), *se);
        }
        ctor = ae ? LookupCtor(EXTERN_INDEXED_UPDATE_CTOR, externTy) : accessCtor;
        args.emplace_back(std::move(receiver));
        args.emplace_back(BuildTree(*se->indexExprs.back()));
    } else {
        auto& ce = StaticCast<CallExpr&>(expr);
        ctor = LookupCtor(EXTERN_FUNCTION_CALL_CTOR, externTy);
        if (!ctor.decl) {
            return nullptr;
        }
        args.emplace_back(BuildTree(*ce.baseFunc));
        args.emplace_back(BuildArgs(ce, *ctor.ty->paramTys[1]));
    }
    if (ae) {
        args.emplace_back(BuildTree(*ae->rightExpr));
    }
    if (!ctor.decl || std::any_of(args.begin(), args.end(), [](auto& arg) { return !arg; })) {
        return nullptr;
    }
    return CreateCtorCall(ctor, externTy, std::move(args), expr);
}

OwnedPtr<Expr> ExternDesugaring::BuildCompoundAssignment(AssignExpr& ae, Ty& externTy)
{
    auto ctor = LookupCtor(EXTERN_COMPOUND_ASSIGNMENT_CTOR, externTy);
    if (!ctor.decl) {
        return nullptr;
    }
    // e.f op= v is built from the access e.f and the binary operator op, without '='.
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

OwnedPtr<CallExpr> ExternDesugaring::CreateCtorCall(
    const Ctor& ctor, Ty& externTy, std::vector<OwnedPtr<Expr>> args, const Node& pos)
{
    CJC_ASSERT(ctor.decl && ctor.ty && ctor.ty->paramTys.size() == args.size());
    auto ctorRef = CreateRefExpr(*ctor.decl);
    ctorRef->SetTy(ctor.ty);
    ctorRef->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&pos, ctorRef.get());
    std::vector<OwnedPtr<FuncArg>> funcArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        auto argTy = args[i]->GetTy();
        auto arg = CreateFuncArg(std::move(args[i]), "", argTy);
        CopyBasicInfo(&pos, arg.get());
        funcArgs.emplace_back(std::move(arg));
    }
    auto ce = CreateCallExpr(
        std::move(ctorRef), std::move(funcArgs), ctor.decl, &externTy, CallKind::CALL_DECLARED_FUNCTION);
    ce->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&pos, ce.get());
    return ce;
}

/**
 * Create the call T.name<instTys>(arg) to the static function @p name of the foreign runtime T of @p externTy, which
 * takes @p argTy and returns @p externTy.
 */
OwnedPtr<CallExpr> ExternDesugaring::CreateRuntimeCall(const std::string& name, OwnedPtr<Expr> arg, Ty& argTy,
    Ty& externTy, std::vector<Ptr<Ty>> instTys, const Expr& pos)
{
    CJC_ASSERT(externTy.IsCoreExternType());
    auto runtimeTy = externTy.typeArgs[0];
    auto [func, matchedParentTy] = lookup(*runtimeTy, name, pos);
    if (!func) {
        return nullptr;
    }
    auto baseFunc = CreateForeignRuntimeFuncAccess(
        *runtimeTy, *func, matchedParentTy, *typeManager.GetFunctionTy({&argTy}, &externTy), pos);
    if (!baseFunc) {
        return nullptr;
    }
    baseFunc->instTys = std::move(instTys);
    std::vector<OwnedPtr<FuncArg>> args;
    args.emplace_back(CreateFuncArg(std::move(arg), "", &argTy));
    auto ce = CreateCallExpr(std::move(baseFunc), std::move(args), func, &externTy, CallKind::CALL_DECLARED_FUNCTION);
    ce->EnableAttr(Attribute::IMPLICIT_ADD);
    CopyBasicInfo(&pos, ce.get());
    return ce;
}
} // namespace

ForeignRuntimeFunc TypeChecker::TypeCheckerImpl::LookupForeignRuntimeFunc(
    ASTContext& ctx, Ty& runtimeTy, const std::string& name, const Expr& pos)
{
    std::vector<Ptr<Decl>> candidates;
    if (runtimeTy.IsGeneric()) {
        auto foreignRuntime = importManager.GetCoreDecl<InterfaceDecl>(STD_LIB_FOREIGN_RUNTIME);
        if (!foreignRuntime) {
            return {};
        }
        candidates = foreignRuntime->GetMemberDeclPtrs();
    } else {
        candidates = FieldLookup(ctx, Ty::GetDeclOfTy(&runtimeTy), name, {&runtimeTy, pos.curFile});
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
        diag.DiagnoseRefactor(DiagKindRefactor::sema_interface_call_with_unimplemented_call, pos, "function", name);
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
    if (!importManager.GetCoreDecl<InterfaceDecl>(STD_LIB_FOREIGN_RUNTIME)) {
        return;
    }
    auto needConversion = [this](Ty& from, Ty& to) { return NeedExternConversion(from, to); };
    auto lookup = [this, &ctx](Ty& runtimeTy, const std::string& name, const Expr& pos) {
        return LookupForeignRuntimeFunc(ctx, runtimeTy, name, pos);
    };
    ExternDesugaring(typeManager, needConversion, lookup).Run(pkg);
}
