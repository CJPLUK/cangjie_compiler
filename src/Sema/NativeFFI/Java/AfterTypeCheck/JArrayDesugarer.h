// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file declares java mirror JArray desugar methods
 */
#ifndef CANGJIE_SEMA_NATIVE_FFI_JAVA_AFTER_TYPE_CHECK_JARRAY_DESUGAR_MANAGER
#define CANGJIE_SEMA_NATIVE_FFI_JAVA_AFTER_TYPE_CHECK_JARRAY_DESUGAR_MANAGER

#include "NativeFFI/Java/AfterTypeCheck/InteropLibBridge.h"
#include "NativeFFI/Java/AfterTypeCheck/Utils.h"
#include "cangjie/AST/Node.h"
#include "cangjie/Modules/ImportManager.h"
#include "cangjie/Sema/TypeManager.h"

namespace Cangjie::Interop::Java {
using namespace Cangjie::AST;

class JArrayDesugarer {
public:
    JArrayDesugarer(TypeManager& typeManager, ImportManager& importManager, InteropLibBridge& lib);

    /**
     * Inserts a new constructor with a jniType parameter.
     * * [ GENERATED CODE ]
     * -------------------------------------------------------------------------
     * init(length: Int32, $jniType: String) {
     *      this({
     *       =>
     *           let $tmp1: CPointer<CPointer<JNINativeInterface_>> = Java_CFFI_get_env()
     *          return unsafe {
     *              Java_CFFI_newJavaArray($tmp1, $jniType, length)
     *          }
     *      }())
     * }
     * -------------------------------------------------------------------------
     */
    void GenerateJniTypeConstructor(ClassDecl& jarray);

    /**
     * Inserts throw Exception("unexpected call") into body of given size constructor.
     * If provided constructor is not exact init(length: Int32), no transformations are performed.
     *
     * ------------------------------------------------------------
     * init(length: Int32) {
     * // throw Exception("unexpected call") will be inserted here
     * }
     * ------------------------------------------------------------
     */
    void InsertOriginalSizeConstructorBody(FuncDecl& constr);

    /**
    * Transforms all java.lang.JArray's constructor calls:
    * init(length: Int32) -> init(lenght: Int32, $jniType: String)
    *
    * example:
    * let u = JArray<User>(1) -> let u = JArray<User>(1, "Lutils/User;")
    *
    * Note: original constructor init(lenght: Int32) must throw Exception
    */
    void TransformConstructorCallsToPassJNIParam(File& file);

private:
    TypeManager& typeManager;
    ImportManager& importManager;
    InteropLibBridge lib;

    void InsertJniTypeParamIntoConstructor(FuncDecl& constr);
    void InsertConstructorBody(FuncDecl& constr);
    Ptr<FuncDecl> FindSizeJNITypeConstructor(ClassDecl& jarray);
    Ptr<FuncDecl> FindSizeJNITypeConstructorFromInnerDecl(const Decl& decl);
};

}; // namespace Cangjie::Interop::Java

#endif
