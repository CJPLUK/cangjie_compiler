// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_CHIR_BASE_H
#define CANGJIE_CHIR_BASE_H

#include <unordered_map>
#include <typeinfo>

#include "cangjie/CHIR/IR/Annotation.h"

namespace Cangjie::CHIR {
class Base {
public:
    template <typename T, typename... Args> void Set(Args&&... args)
    {
        anno.Set<T>(std::forward<Args>(args)...);
    }

    template <typename T> void Remove()
    {
        anno.Remove<T>();
    }

    // Get the value of the annotation T associated to this node
    template <typename T>
    decltype(std::declval<const AnnotationMap>().Get<T>()) Get() const
    {
        return anno.Get<T>();
    }

    template <class T> T& GetAnno()
    {
        return anno.GetAnno<T>();
    }

    virtual const DebugLocation& GetDebugLocation() const;
    void SetDebugLocation(const DebugLocation& newLoc);
    void SetDebugLocation(DebugLocation&& newLoc);
    void CopyBaseInfoFrom(const Base& other);
    std::string BaseCommentToString() const;
    const AnnotationMap& GetAnno() const;

    virtual ~Base() = default;

private:
    AnnotationMap anno;
    DebugLocation loc{};
};
}
#endif
