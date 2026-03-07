// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements codegen for CHIR Invoke.
 */

#include "Base/InvokeImpl.h"

#include "Base/CHIRExprWrapper.h"
#include "CGModule.h"
#include "IRBuilder.h"
#include "cangjie/CHIR/Expression/Terminator.h"
#include "cangjie/CHIR/Type/ClassDef.h"
#include "cangjie/CHIR/Type/Type.h"

using namespace Cangjie;
using namespace CodeGen;
using namespace CHIR;

namespace {
llvm::MDTuple* GenObjTyMetaForVirtualCall(IRBuilder2& irBuilder, const CHIR::Type& ty)
{
    CJC_ASSERT(ty.GetTypeArgs().size() <= 1);
    llvm::LLVMContext& llvmCtx = irBuilder.GetCGContext().GetLLVMContext();
    std::vector<llvm::Metadata*> objTyMeta{};
    if (ty.GetTypeArgs().size() == 1) {
        objTyMeta = UnwindGenericRelateType(llvmCtx, ty);
    } else {
        std::string tiName = CGType::GetNameOfTypeInfoGV(ty);
        objTyMeta.emplace_back(llvm::MDString::get(llvmCtx, tiName));
    }
    return llvm::MDTuple::get(llvmCtx, objTyMeta);
}

bool VirtualCallCanBeHoisted(IRBuilder2& irBuilder, const CHIRCallExpr& callExprWrapper)
{
    if (irBuilder.GetCGContext().GetCompileOptions().optimizationLevel < GlobalOptions::OptimizationLevel::O2) {
        return false;
    }
    return callExprWrapper.GetThisType(false)->IsGeneric();
}
} // namespace

llvm::Value* CodeGen::GenerateInvoke(IRBuilder2& irBuilder, CHIRInvokeWrapper invoke)
{
    irBuilder.SetCHIRExpr(&invoke);
    auto& cgMod = irBuilder.GetCGModule();
    auto objVal = cgMod | invoke.GetObject();
    std::vector<CGValue*> argsVal;
    for (auto arg : invoke.GetArgs()) {
        (void)argsVal.emplace_back(cgMod | arg);
    }

    auto funcType = invoke.GetMethodType();
    auto& cgCtx = cgMod.GetCGContext();

    auto objType = DeRef(*invoke.GetObject()->GetType());
    llvm::Value* funcPtr = nullptr;
    if (!objType->IsAutoEnv()) {
        if (VirtualCallCanBeHoisted(irBuilder, invoke)) {
            auto [prepForVirtualCallBB] =
                Vec2Tuple<1>(irBuilder.CreateAndInsertBasicBlocks({GenNameForBB("prepForVirtualCall.bb")}));
            invoke.SetPrepForVirtualCallBB(prepForVirtualCallBB);
            irBuilder.CreateBr(prepForVirtualCallBB);
            irBuilder.SetInsertPoint(prepForVirtualCallBB);
        }
        auto ti = irBuilder.GetTypeInfoFromObject(objVal->GetRawValue());
        auto vtableOffset = invoke.GetVirtualMethodOffset();
        auto idxOfVFunc = irBuilder.getInt64(vtableOffset);
        auto introType = StaticCast<const CHIR::ClassType*>(invoke.GetOuterType(cgCtx.GetCHIRBuilder()));
        if (introType->GetClassDef()->IsInterface()) {
            auto introTi = irBuilder.CreateTypeInfo(introType);
            funcPtr = irBuilder.CallIntrinsicMTable({ti, introTi, idxOfVFunc});
        } else {
            auto vtableSizeOfIntroType = cgCtx.GetVTableSizeOf(introType);
            CJC_ASSERT(vtableSizeOfIntroType > 0);
            auto idxOfIntroType = irBuilder.getInt64(vtableSizeOfIntroType - 1U);
            auto introTi = irBuilder.CreateTypeInfo(introType);
            funcPtr = irBuilder.CallIntrinsicGetVTableFunc(ti, idxOfIntroType, idxOfVFunc, introTi);
            auto& llvmCtx = cgCtx.GetLLVMContext();
            auto meta = llvm::MDTuple::get(llvmCtx, llvm::MDString::get(llvmCtx, GetTypeQualifiedName(*introType)));
            llvm::cast<llvm::Instruction>(funcPtr)->setMetadata("IntroType", meta);
        }
        if (introType->GetTypeArgs().size() <= 1) {
            auto objTyMeta = GenObjTyMetaForVirtualCall(irBuilder, *introType);
            llvm::cast<llvm::Instruction>(funcPtr)->setMetadata("objType", objTyMeta);
        }
        if (auto prepForVirtualCallBB = invoke.GetPrepForVirtualCallBB(); prepForVirtualCallBB) {
            cgCtx.AddVirtualCallInfo4LICM(irBuilder.GetInsertCGFunction(),
                {prepForVirtualCallBB, funcPtr, invoke.GetThisType(false), introType, vtableOffset});
            auto [prepForVirtualCallEndBB] =
                Vec2Tuple<1>(irBuilder.CreateAndInsertBasicBlocks({GenNameForBB("prepForVirtualCall.end")}));
            irBuilder.CreateBr(prepForVirtualCallEndBB);
            irBuilder.SetInsertPoint(prepForVirtualCallEndBB);
        }
    } else {
        auto i8PtrTy = irBuilder.getInt8PtrTy();
        auto funcName = invoke.GetMethodName();
        auto methodIdx = CHIR::GetMethodIdxInAutoEnvObject(funcName);
        auto payload = irBuilder.GetPayloadFromObject(**objVal);
        auto vritualPtr = irBuilder.CreateConstGEP1_32(i8PtrTy,
            irBuilder.LLVMIRBuilder2::CreateBitCast(payload, i8PtrTy->getPointerTo(1U)),
            static_cast<unsigned>(methodIdx), "virtualFPtr");
        auto loadInst = irBuilder.LLVMIRBuilder2::CreateLoad(i8PtrTy, vritualPtr);
        loadInst->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(cgMod.GetLLVMContext(), {}));
        funcPtr = llvm::cast<llvm::Value>(loadInst);
    }
    auto concreteFuncType = static_cast<CGFunctionType*>(CGType::GetOrCreate(
        cgMod, funcType, CGType::TypeExtraInfo{0, true, false, true, invoke.GetInstantiatedTypeArgs()}));
    funcPtr = irBuilder.CreateBitCast(funcPtr, concreteFuncType->GetLLVMFunctionType()->getPointerTo());
    auto result = irBuilder.CreateCallOrInvoke(*concreteFuncType, funcPtr, argsVal);

    auto thisValue = **(cgMod | invoke.GetThisParam());
    auto originalNonRefVal = cgCtx.GetOriginalNonRefValOfBoxedValue(thisValue);
    if (originalNonRefVal && invoke.TestVritualMethodAttr(cgCtx.GetCHIRBuilder(), CHIR::Attribute::MUT)) {
        auto [handleBoxedBB, handleEndBB] =
            Vec2Tuple<2>(irBuilder.CreateAndInsertBasicBlocks({"handle_boxed", "handle_end"}));
        auto thisValueTI = irBuilder.GetTypeInfoFromObject(thisValue);
        irBuilder.CreateCondBr(irBuilder.CreateTypeInfoIsReferenceCall(thisValueTI), handleEndBB, handleBoxedBB);

        irBuilder.SetInsertPoint(handleBoxedBB);
        auto basePtr = cgCtx.GetBasePtrOf(originalNonRefVal);
        auto sizeOfTI = irBuilder.GetSizeFromTypeInfo(thisValueTI);
        std::vector<llvm::Value*> copyGenericParams{basePtr, originalNonRefVal, thisValue, sizeOfTI};
        irBuilder.CallIntrinsicGCWriteGeneric(copyGenericParams);
        irBuilder.CreateBr(handleEndBB);

        irBuilder.SetInsertPoint(handleEndBB);
    }
    return result;
}

llvm::Value* CodeGen::GenerateInvokeStatic(IRBuilder2& irBuilder, CHIRInvokeStaticWrapper invokeStatic)
{
    irBuilder.SetCHIRExpr(&invokeStatic);
    auto& cgMod = irBuilder.GetCGModule();
    std::vector<CGValue*> argsVal{};
    for (auto arg : invokeStatic.GetArgs()) {
        (void)argsVal.emplace_back(cgMod | arg);
    }

    auto& cgCtx = cgMod.GetCGContext();
    llvm::Value* funcPtr = nullptr;
    if (VirtualCallCanBeHoisted(irBuilder, invokeStatic)) {
        auto [prepForVirtualCallBB] =
            Vec2Tuple<1>(irBuilder.CreateAndInsertBasicBlocks({GenNameForBB("prepForVirtualCall.bb")}));
        invokeStatic.SetPrepForVirtualCallBB(prepForVirtualCallBB);
        irBuilder.CreateBr(prepForVirtualCallBB);
        irBuilder.SetInsertPoint(prepForVirtualCallBB);
    }
    llvm::Value* ti = **(cgMod | invokeStatic.GetRTTIValue());
    auto vtableOffset = invokeStatic.GetVirtualMethodOffset();
    auto idxOfVFunc = irBuilder.getInt64(vtableOffset);
    auto introType = StaticCast<const CHIR::ClassType*>(invokeStatic.GetOuterType(cgCtx.GetCHIRBuilder()));
    if (introType->GetClassDef()->IsInterface()) {
        auto introTi = irBuilder.CreateTypeInfo(introType);
        funcPtr = irBuilder.CallIntrinsicMTable({ti, introTi, idxOfVFunc});
    } else {
        auto vtableSizeOfIntroType = cgCtx.GetVTableSizeOf(introType);
        CJC_ASSERT(vtableSizeOfIntroType > 0);
        auto idxOfIntroType = irBuilder.getInt64(vtableSizeOfIntroType - 1U);
        auto introTi = irBuilder.CreateTypeInfo(introType);
        funcPtr = irBuilder.CallIntrinsicGetVTableFunc(ti, idxOfIntroType, idxOfVFunc, introTi);
        auto& llvmCtx = cgCtx.GetLLVMContext();
        auto meta = llvm::MDTuple::get(llvmCtx, llvm::MDString::get(llvmCtx, GetTypeQualifiedName(*introType)));
        llvm::cast<llvm::Instruction>(funcPtr)->setMetadata("IntroType", meta);
    }
    if (introType->GetTypeArgs().size() <= 1) {
        auto objTyMeta = GenObjTyMetaForVirtualCall(irBuilder, *introType);
        llvm::cast<llvm::Instruction>(funcPtr)->setMetadata("objType", objTyMeta);
    }
    if (auto prepForVirtualCallBB = invokeStatic.GetPrepForVirtualCallBB(); prepForVirtualCallBB) {
        cgCtx.AddVirtualCallInfo4LICM(irBuilder.GetInsertCGFunction(),
            {prepForVirtualCallBB, funcPtr, invokeStatic.GetThisType(false), introType, vtableOffset});
        auto [prepForVirtualCallEndBB] =
            Vec2Tuple<1>(irBuilder.CreateAndInsertBasicBlocks({GenNameForBB("prepForVirtualCall.end")}));
        irBuilder.CreateBr(prepForVirtualCallEndBB);
        irBuilder.SetInsertPoint(prepForVirtualCallEndBB);
    }

    auto funcType = invokeStatic.GetMethodType();
    auto concreteFuncType = static_cast<CGFunctionType*>(CGType::GetOrCreate(
        cgMod, funcType, CGType::TypeExtraInfo{0, true, true, false, invokeStatic.GetInstantiatedTypeArgs()}));
    funcPtr = irBuilder.CreateBitCast(funcPtr, concreteFuncType->GetLLVMFunctionType()->getPointerTo());
    return irBuilder.CreateCallOrInvoke(*concreteFuncType, funcPtr, argsVal, false, ti);
}
