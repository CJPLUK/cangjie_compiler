// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements type check cache utils.
 */

#include "cangjie/AST/Cache.h"
#include "cangjie/Utils/Casting.h"

using namespace Cangjie;
using namespace AST;

namespace Cangjie::AST {
TargetCache CollectTargets(const Node& node, const std::function<bool(Ty&)>& isExternTy)
{
    if (auto farg = DynamicCast<const FuncArg*>(&node)) {
        return CollectTargets(*farg->expr, isExternTy);
    }
    Ptr<Decl> target1 = node.GetTarget();
    // `RestoreTargets` always writes the member-access base target back, so it must be captured here.
    // Normally that only happens when the access itself resolved to a target; additionally capture it
    // for a dynamic `Extern` member access (`e.foo`, read or assignment target), which has no member
    // target of its own. Otherwise the early `{target1, null}` result would make `RestoreTargets` wipe
    // the resolved base reference's target on a cache restore, leaving `e` unresolved for the deferred
    // extern desugaring / CHIR. The `Extern` test keeps this special case off every other access.
    if (auto ma = DynamicCast<const MemberAccess*>(&node); ma && ma->baseExpr && ma->baseExpr->IsReferenceExpr()) {
        auto baseTy = ma->baseExpr->GetTy();
        bool externBase = baseTy && isExternTy && isExternTy(*baseTy);
        if (target1 || externBase) {
            return std::make_pair(target1, ma->baseExpr->GetTarget());
        }
    }
    return std::make_pair(target1, nullptr);
}

void RestoreTargets(Node& node, const TargetCache& targets)
{
    if (auto farg = DynamicCast<const FuncArg*>(&node)) {
        RestoreTargets(*farg->expr, targets);
    }
    node.SetTarget(targets.first);
    if (auto ma = DynamicCast<const MemberAccess*>(&node);
           ma && ma->baseExpr && ma->baseExpr->IsReferenceExpr()) {
        ma->baseExpr->SetTarget(targets.second);
    }
}

bool CacheKey::operator==(const CacheKey& b) const
{
    return target == b.target && isDesugared == b.isDesugared && diagKey == b.diagKey;
}

bool MemSig::operator==(const MemSig& b) const
{
    return id == b.id && isVarOrProp == b.isVarOrProp && arity == b.arity && genArity == b.genArity;
}
} // namespace Cangjie::AST
