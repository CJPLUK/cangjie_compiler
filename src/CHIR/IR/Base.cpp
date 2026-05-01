// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "cangjie/CHIR/IR/Base.h"

#include "cangjie/CHIR/Utils/ToStringUtils.h"

using namespace Cangjie::CHIR;

const DebugLocation& Base::GetDebugLocation() const
{
    return loc;
}

void Base::SetDebugLocation(const DebugLocation& newLoc)
{
    loc = newLoc;
}

void Base::SetDebugLocation(DebugLocation&& newLoc)
{
    loc = std::move(newLoc);
}

void Base::CopyBaseInfoFrom(const Base& other)
{
    anno = other.anno;
    loc = other.loc;
}

std::string Base::BaseCommentToString() const
{
    std::vector<std::string> result;
    result.emplace_back(loc.ToString());
    result.emplace_back(anno.ToString());
    return StringJoin(result, ", ");
}

const AnnotationMap& Base::GetAnno() const
{
    return anno;
}