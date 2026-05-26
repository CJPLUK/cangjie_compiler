// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "NativeFFI/Java/AfterTypeCheck/JArrayDesugarer.h"
#include "NativeFFI/Java/AfterTypeCheck/InteropLibBridge.h"
#include "NativeFFI/Java/AfterTypeCheck/Utils.h"
#include "NativeFFI/Utils.h"
#include "cangjie/AST/AttributePack.h"
#include "cangjie/AST/Clone.h"
#include "cangjie/AST/Create.h"
#include "cangjie/AST/Match.h"
#include "cangjie/AST/Node.h"
#include "cangjie/AST/Types.h"
#include "cangjie/AST/Walker.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Sema/TypeManager.h"
#include "cangjie/Utils/CastingTemplate.h"
#include "cangjie/Utils/CheckUtils.h"
#include "cangjie/Utils/SafePointer.h"
#include <utility>

namespace {
using namespace Cangjie;
using namespace Cangjie::AST;
using namespace Cangjie::Interop::Java;

bool IsJArrayConstructor(const FuncDecl& constr)
{
    bool isConstructor = constr.TestAttr(Attribute::CONSTRUCTOR);
    bool isParentJArray = constr.outerDecl && IsJArray(*constr.outerDecl);

    return isConstructor && isParentJArray;
}

bool IsSizeConstructor(const FuncDecl& constr)
{
    return IsJArrayConstructor(constr) && AreParamTypeKindsValid(constr, {TypeKind::TYPE_INT32});
}

bool IsSizeJNITypeConstructor(const FuncDecl& constr, const ImportManager& importManager)
{
    static auto strTy = GetStringDecl(importManager).GetTy();
    CJC_NULLPTR_CHECK(strTy);
    auto isSecondParamStr = [&constr] { return constr.funcBody->paramLists[0]->params[1]->GetTy() == strTy; };

    return IsJArrayConstructor(constr) &&
        AreParamTypeKindsValid(constr, {TypeKind::TYPE_INT32, TypeKind::TYPE_STRUCT}) && isSecondParamStr() &&
        constr.TestAttr(Attribute::COMPILER_ADD);
}

} // namespace

namespace Cangjie::Interop::Java {

JArrayDesugarer::JArrayDesugarer(TypeManager& typeManager, ImportManager& importManager, InteropLibBridge& lib)
    : typeManager(typeManager), importManager(importManager), lib(lib)
{
}

void JArrayDesugarer::GenerateJniTypeConstructor(ClassDecl& jarray)
{
    CJC_ASSERT_WITH_MSG(IsJArray(jarray), "Expected JArray decl");

    OwnedPtr<FuncDecl> newConstr;

    for (auto& member : jarray.GetMemberDecls()) {
        if (const auto constr = DynamicCast<FuncDecl*>(member.get()); constr && IsSizeConstructor(*constr)) {
            newConstr = ASTCloner::Clone(Ptr(constr));
            InsertJniTypeParamIntoConstructor(*newConstr);
            InsertConstructorBody(*newConstr);
            break;
        }
    }

    jarray.GetMemberDecls().emplace_back(std::move(newConstr));
}

void JArrayDesugarer::InsertOriginalSizeConstructorBody(FuncDecl& constr)
{
    if (IsSizeConstructor(constr)) {
        auto exceptionCall = CreateThrowExceptionCall(importManager, typeManager, "unexpected call", constr.curFile);
        constr.funcBody->body->body.emplace_back(std::move(exceptionCall));
    }
}

void JArrayDesugarer::TransformConstructorCallsToPassJNIParam(File& file)
{
    Walker(&file, Walker::GetNextWalkerID(), [this, &file](auto node) {
        CJC_ASSERT_WITH_MSG(file.curPackage, "file's curPackage is nullptr");
        if (!node->IsSamePackage(*file.curPackage)) { // if node is imported
            return VisitAction::WALK_CHILDREN;
        }

        Ptr<CallExpr> callExpr = As<ASTKind::CALL_EXPR>(node);
        if (!callExpr) {
            return VisitAction::WALK_CHILDREN;
        }

        auto funcDecl = callExpr->resolvedFunction;
        if (!funcDecl || !IsSizeConstructor(*funcDecl)) {
            return VisitAction::WALK_CHILDREN;
        }

        // here funcDecl is size constructor of JArray

        static auto jniTypeConstr = FindSizeJNITypeConstructorFromInnerDecl(*funcDecl);
        CJC_NULLPTR_CHECK(jniTypeConstr);

        auto newCallExpr = ASTCloner::Clone(callExpr);
        newCallExpr->resolvedFunction = jniTypeConstr;

        Ptr<Ty> jarrayElementType;
        { // update newCallExpr->baseFunc
            if (auto base = As<ASTKind::REF_EXPR>(newCallExpr->baseFunc)) {
                CJC_ASSERT_WITH_MSG(!base->typeArguments.empty(), "JArray type must be generic");
                jarrayElementType = base->typeArguments[0]->GetTy();
                base->ref.target = jniTypeConstr;
                base->SetTy(jniTypeConstr->GetTy());
            } else {
                CJC_ABORT_WITH_MSG("constructor call of java.lang.JArray expected to be RefExpr");
            }
        }

        CJC_ASSERT_WITH_MSG(newCallExpr->args.size() == 1, "expected to be init(length: Int32)");
        { // add jniType param in new constructor call
            auto jniType = lib.SelectJSigByTypeKind(jarrayElementType->kind, jarrayElementType);
            auto fa = CreateFuncArg(std::move(jniType));
            newCallExpr->args.emplace_back(std::move(fa));
        }

        // update desugarArgs as it has value of cloned init(length: Int32)
        newCallExpr->desugarArgs = [&] {
            std::vector<Ptr<FuncArg>> argPtrs;
            for (auto& arg : newCallExpr->args) {
                argPtrs.emplace_back(arg.get());
            }
            return argPtrs;
        }();
        callExpr->desugarExpr = std::move(newCallExpr);

        return VisitAction::WALK_CHILDREN;
    }).Walk();
}

void JArrayDesugarer::InsertJniTypeParamIntoConstructor(FuncDecl& constr)
{
    CJC_ASSERT_WITH_MSG(IsJArray(*constr.outerDecl), "Expected JArray decl");
    CJC_ASSERT_WITH_MSG(constr.TestAttr(Attribute::CONSTRUCTOR), "'constr' argument expected to be constructor");

    auto strTy = GetStringDecl(importManager).GetTy();

    { // add new func param
        auto jniTypeParam = CreateFuncParam("$jniType", CreateType(strTy), nullptr, strTy);
        constr.funcBody->paramLists[0]->params.emplace_back(std::move(jniTypeParam));
    }

    { // add new param ty in constr.ty->paramTys
        std::vector<Ptr<Ty>> ctorFuncParamTys;
        if (auto fty = DynamicCast<FuncTy*>(constr.GetTy())) {
            ctorFuncParamTys = std::move(fty->paramTys);
        } else {
            CJC_ABORT_WITH_MSG("'fd.ty' expected to be 'FuncTy'");
        }
        ctorFuncParamTys.push_back(strTy);
        constr.SetTy(typeManager.GetFunctionTy(std::move(ctorFuncParamTys), constr.outerDecl->GetTy()));
    }
}

void JArrayDesugarer::InsertConstructorBody(FuncDecl& constr)
{
    CJC_ASSERT_WITH_MSG(IsJArray(*constr.outerDecl), "Expected JArray decl");

    auto jniEnvCall = lib.CreateGetJniEnvCall(constr.curFile);
    auto jniEnvPtrDecl = lib.GetJniEnvPtrDecl();

    if (!jniEnvCall || !jniEnvCall) {
        constr.EnableAttr(Attribute::IS_BROKEN);
        return;
    }

    auto jniEnvVar = CreateTmpVarDecl(jniEnvPtrDecl->type, jniEnvCall);
    auto newObjectCall = lib.CreateCFFINewJavaArrayCall(
        WithinFile(CreateRefExpr(*jniEnvVar), constr.curFile), *constr.funcBody->paramLists[0]);
    std::vector<OwnedPtr<Node>> lambdaNodes = Nodes(std::move(jniEnvVar), std::move(newObjectCall));

    constr.constructorCall = ConstructorCall::OTHER_INIT;

    if (auto jarray = DynamicCast<ClassLikeDecl*>(constr.outerDecl)) {
        static auto generatedJavaRefInitConstr = GetGeneratedJavaMirrorConstructor(*jarray);
        auto thisCall = CreateThisCall(
            *constr.outerDecl, *generatedJavaRefInitConstr, generatedJavaRefInitConstr->GetTy(), constr.curFile);
        thisCall->args.push_back(CreateFuncArg(WrapReturningLambdaCall(typeManager, std::move(lambdaNodes))));
        constr.funcBody->body->body.push_back(std::move(thisCall));
    } else {
        CJC_ABORT_WITH_MSG("Jarray expected to be ClassLikeDecl");
    }
}

Ptr<FuncDecl> JArrayDesugarer::FindSizeJNITypeConstructor(ClassDecl& jarray)
{
    CJC_ASSERT_WITH_MSG(IsJArray(jarray), "Expected JArray decl");

    for (auto& member : jarray.GetMemberDecls()) {
        if (auto fd = DynamicCast<FuncDecl*>(&*member)) {
            if (IsSizeJNITypeConstructor(*fd, importManager)) {
                return fd;
            }
        }
    }

    return nullptr;
}

Ptr<FuncDecl> JArrayDesugarer::FindSizeJNITypeConstructorFromInnerDecl(const Decl& decl)
{
    if (auto jarray = As<ASTKind::CLASS_DECL>(decl.outerDecl)) {
        return FindSizeJNITypeConstructor(*jarray);
    } else {
        CJC_ABORT_WITH_MSG("'outerDecl' expected to be java.lang.JArray");
        return nullptr;
    }
}
} // namespace Cangjie::Interop::Java
