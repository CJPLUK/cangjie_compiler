// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements the CHIRBuilder class in CHIR.
 */

#include "cangjie/CHIR/IR/CHIRBuilder.h"
#include "cangjie/CHIR/IR/CHIRContext.h"

#include "cangjie/Basic/Print.h"
#include "cangjie/CHIR/Utils/CHIRCasting.h"
#include "cangjie/CHIR/IR/Package.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/EnumDef.h"
#include "cangjie/CHIR/IR/Type/StructDef.h"
#include "cangjie/CHIR/Utils/Utils.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Mangle/CHIRMangler.h"
#include "cangjie/Utils/CheckUtils.h"

using namespace Cangjie::CHIR;

CHIRBuilder::CHIRBuilder(CHIRContext& context, size_t threadIdx) : context(context), threadIdx(threadIdx)
{
}

CHIRBuilder::~CHIRBuilder()
{
    MergeAllocatedInstance();
}

// ===--------------------------------------------------------------------=== //
// BlockGroup API
// ===--------------------------------------------------------------------=== //
BlockGroup* CHIRBuilder::CreateBlockGroup(Func& func)
{
    auto blockGroup = new BlockGroup(std::to_string(func.GenerateBlockGroupId()));
    StoreAllocatedPtrInFuncOrLambda(*blockGroup);
    return blockGroup;
}

// ===--------------------------------------------------------------------===//
// Block API
// ===--------------------------------------------------------------------===//
Block* CHIRBuilder::CreateBlock(BlockGroup* parentGroup)
{
    CJC_NULLPTR_CHECK(parentGroup);
    auto func = parentGroup->GetTopLevelFunc();
    CJC_NULLPTR_CHECK(func);
    std::string idstr = "#" + std::to_string(func->GenerateBlockId());
    auto basicBlock = new Block(idstr, parentGroup);
    auto bg = basicBlock->GetFuncOrLambdaBody();
    StoreAllocatedPtrInFuncOrLambda(*bg, basicBlock);
    if (markAsCompileTimeValue) {
        basicBlock->EnableAttr(Attribute::CONST);
    }
    return basicBlock;
}

// split one block to two blocks, and remove separator
std::pair<Block*, Block*> CHIRBuilder::SplitBlock(const Expression& separator)
{
    auto block1 = separator.GetParentBlock();
    auto block2 = CreateBlock(block1->GetParentBlockGroup());
    bool needMove = false;
    for (auto expr : block1->GetExpressions()) {
        if (expr == &separator) {
            needMove = true;
            expr->RemoveSelfFromBlock();
            auto term = CreateTerminator<GoTo>(block2, block1);
            block1->AppendExpression(term);
            continue;
        }
        if (needMove) {
            expr->MoveTo(*block2);
        }
    }
    return std::pair<Block*, Block*>{block1, block2};
}

// ===--------------------------------------------------------------------===//
// Value API
// ===--------------------------------------------------------------------===//

Parameter* CHIRBuilder::CreateParameter(Type* ty, const DebugLocation& loc, Func& parentFunc)
{
    auto id = parentFunc.GenerateLocalId();
    auto param = new Parameter(ty, "%" + std::to_string(id), &parentFunc);
    param->EnableAttr(Attribute::READONLY);
    param->SetDebugLocation(loc);
    this->allocatedValues.push_back(param);
    return param;
}

Parameter* CHIRBuilder::CreateParameter(Type* ty, const DebugLocation& loc, Lambda& parentLambda)
{
    auto topFunc = parentLambda.GetTopLevelFunc();
    CJC_NULLPTR_CHECK(topFunc);
    auto id = topFunc->GenerateLocalId();
    auto param = new Parameter(ty, "%" + std::to_string(id), parentLambda);
    param->EnableAttr(Attribute::READONLY);
    param->SetDebugLocation(loc);
    this->allocatedValues.push_back(param);
    return param;
}

namespace {
// Helper function to check if new features is a superset of old features
// and update the value's features and PREVIOUSLY_DESERIALIZED attribute accordingly
template <typename T>
void TryUpdateExistingValue(T* existing, const std::set<std::string>& newFeatures)
{
    auto oldFeatures = existing->GetFeatures();
    bool newIsSuperSet = std::includes(newFeatures.begin(), newFeatures.end(),
        oldFeatures.begin(), oldFeatures.end()) && newFeatures.size() > oldFeatures.size();
    if (!newIsSuperSet) {
        // already existed value more specific, so no need to update
        existing->EnableAttr(Attribute::PREVIOUSLY_DESERIALIZED);
    } else {
        // will be updated as it is loaded first time
        existing->DisableAttr(Attribute::PREVIOUSLY_DESERIALIZED);
        existing->SetFeatures(newFeatures);
    }
}

} // namespace

GlobalVar* CHIRBuilder::CreateGlobalVar(const DebugLocation& loc, RefType* ty, const std::string& mangledName,
    const std::string& srcCodeIdentifier, const std::string& rawMangledName, const std::string& packageName,
    std::set<std::string> features)
{
    auto identifier = "@" + mangledName;
    GlobalVar* globalVar = nullptr;
    if (compileCJMP && context.GetCurPackage() != nullptr) {
        if (auto exist = context.GetCurPackage()->TryGetGlobalVar(identifier)) {
            globalVar = exist;
            // Update features set
            TryUpdateExistingValue(globalVar, features);
        }
    }
    if (globalVar == nullptr) {
        globalVar = new GlobalVar(ty, identifier, srcCodeIdentifier, rawMangledName, packageName);
        globalVar->SetFeatures(features);
        this->allocatedValues.push_back(globalVar);
        if (context.GetCurPackage() != nullptr) {
            context.GetCurPackage()->AddGlobalVar(globalVar);
        }
    }
    globalVar->SetDebugLocation(loc);
    return globalVar;
}

// ===--------------------------------------------------------------------===//
// Expression API
// ===--------------------------------------------------------------------===//

Func* CHIRBuilder::CreateFunc(const DebugLocation& loc, FuncType* funcTy, const std::string& mangledName,
    const std::string& srcCodeIdentifier, const std::string& rawMangledName, const std::string& packageName,
    const std::vector<GenericType*>& genericTypeParams, std::set<std::string> features)
{
    auto identifier = "@" + mangledName;
    Func* func = nullptr;
    if (compileCJMP && context.GetCurPackage() != nullptr) {
        if (auto exist = context.GetCurPackage()->TryGetGlobalFunc(identifier)) {
            func = exist;
            // Update features set
            TryUpdateExistingValue(func, features);
        }
    }
    if (func == nullptr) {
        func = new Func(
            funcTy, identifier, srcCodeIdentifier, rawMangledName, packageName, genericTypeParams);
        func->SetFeatures(features);
        this->allocatedValues.push_back(func);
        if (context.GetCurPackage() != nullptr) {
            context.GetCurPackage()->AddGlobalFunc(func);
        }
    }
    func->SetDebugLocation(loc);
    return func;
}

// ===--------------------------------------------------------------------===//
// StructDef API
// ===--------------------------------------------------------------------===//
StructDef* CHIRBuilder::CreateStruct(const DebugLocation& loc, const std::string& srcCodeIdentifier,
    const std::string& mangledName, const std::string& pkgName, bool isImported)
{
    StructDef* ret = new StructDef(srcCodeIdentifier, "@" + mangledName, pkgName);
    this->allocatedStructs.push_back(ret);
    if (context.GetCurPackage() != nullptr) {
        if (isImported) {
            context.GetCurPackage()->AddImportedStruct(ret);
            ret->EnableAttr(Attribute::IMPORTED);
        } else {
            context.GetCurPackage()->AddStruct(ret);
        }
    }
    ret->SetDebugLocation(loc);
    return ret;
}
// ===--------------------------------------------------------------------===//
// ClassDef API
// ===--------------------------------------------------------------------===//
ClassDef* CHIRBuilder::CreateClass(const DebugLocation& loc, const std::string& srcCodeIdentifier,
    const std::string& mangledName, const std::string& pkgName, bool isClass, bool isImported)
{
    ClassDef* ret = new ClassDef(srcCodeIdentifier, "@" + mangledName, pkgName, isClass);
    this->allocatedClasses.push_back(ret);
    if (context.GetCurPackage() != nullptr) {
        if (isImported) {
            context.GetCurPackage()->AddImportedClass(ret);
            ret->EnableAttr(Attribute::IMPORTED);
        } else {
            context.GetCurPackage()->AddClass(ret);
        }
    }
    ret->SetDebugLocation(loc);
    return ret;
}
// ===--------------------------------------------------------------------===//
// EnumDef API
// ===--------------------------------------------------------------------===//
EnumDef* CHIRBuilder::CreateEnum(const DebugLocation& loc, const std::string& srcCodeIdentifier,
    const std::string& mangledName, const std::string& pkgName, bool isImported, bool isNonExhaustive)
{
    EnumDef* ret = new EnumDef(srcCodeIdentifier, "@" + mangledName, pkgName, isNonExhaustive);
    this->allocatedEnums.push_back(ret);
    if (context.GetCurPackage() != nullptr) {
        if (isImported) {
            context.GetCurPackage()->AddImportedEnum(ret);
            ret->EnableAttr(Attribute::IMPORTED);
        } else {
            context.GetCurPackage()->AddEnum(ret);
        }
    }
    ret->SetDebugLocation(loc);
    return ret;
}
// ===--------------------------------------------------------------------===//
// ExtendDef API
// ===--------------------------------------------------------------------===//
ExtendDef* CHIRBuilder::CreateExtend(const DebugLocation& loc, const std::string& mangledName,
    const std::string& pkgName, bool isImported, const std::vector<GenericType*> genericParams)
{
    ExtendDef* ret = new ExtendDef("@" + mangledName, pkgName, genericParams);
    this->allocatedExtends.emplace_back(ret);
    if (context.GetCurPackage() != nullptr) {
        if (isImported) {
            context.GetCurPackage()->AddImportedExtend(ret);
            ret->EnableAttr(Attribute::IMPORTED);
        } else {
            context.GetCurPackage()->AddExtend(ret);
        }
    }
    ret->SetDebugLocation(loc);
    return ret;
}
// ===--------------------------------------------------------------------===//
// Package API
// ===--------------------------------------------------------------------===//
Package* CHIRBuilder::CreatePackage(const std::string& name)
{
    Package* pkg = new Package(name);
    context.SetCurPackage(pkg);
    return pkg;
}

Package* CHIRBuilder::GetCurPackage() const
{
    return context.GetCurPackage();
}

std::unordered_set<CustomType*> CHIRBuilder::GetAllCustomTypes() const
{
    std::unordered_set<CustomType*> result;
    for (auto ty : context.dynamicAllocatedTys) {
        if (auto customTy = DynamicCast<CustomType*>(ty); customTy) {
            result.emplace(customTy);
        }
    }
    for (auto ty : context.constAllocatedTys) {
        if (auto customTy = DynamicCast<CustomType*>(ty); customTy) {
            result.emplace(customTy);
        }
    }
    return result;
}

std::unordered_set<GenericType*> CHIRBuilder::GetAllGenericTypes() const
{
    std::unordered_set<GenericType*> result;
    for (auto ty : context.dynamicAllocatedTys) {
        if (auto genericTy = DynamicCast<GenericType*>(ty); genericTy) {
            result.emplace(genericTy);
        }
    }
    for (auto ty : context.constAllocatedTys) {
        if (auto genericTy = DynamicCast<GenericType*>(ty); genericTy) {
            result.emplace(genericTy);
        }
    }
    return result;
}

void CHIRBuilder::EnableIRCheckerAfterPlugin()
{
    enableIRCheckerAfterPlugin = true;
}

void CHIRBuilder::DisableIRCheckerAfterPlugin()
{
    enableIRCheckerAfterPlugin = false;
}

bool CHIRBuilder::IsEnableIRCheckerAfterPlugin() const
{
    return enableIRCheckerAfterPlugin;
}

void CHIRBuilder::StoreAllocatedPtrInFuncOrLambda(BlockGroup& bg, Base* ptr)
{
    auto it = allocatedPtrInFuncOrLambda.find(&bg);
    if (it == allocatedPtrInFuncOrLambda.end()) {
        if (ptr == nullptr) {
            allocatedPtrInFuncOrLambda.emplace(&bg, std::vector<Base*>{});
        } else {
            allocatedPtrInFuncOrLambda.emplace(&bg, std::vector<Base*>{ptr});
        }
    } else {
        it->second.emplace_back(ptr);
    }
}