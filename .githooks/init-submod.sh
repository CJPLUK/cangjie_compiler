#!/bin/bash

[ "e" != "e$1" ] || exit 1 # first argument cannot be empty

gitcommondir=$(git rev-parse --git-common-dir)

existingsubmodules()
{
    # $1 is relative path of submodule
    (echo $gitcommondir; ls $gitcommondir/worktrees) \
    | sed -e "s;\$;/modules/$1/;" \
    | while read possiblyexisting
        do [ -d $possiblyexisting ] && (
            isshallow=$(unset $(git rev-parse --local-env-vars); git -C $possiblyexisting rev-parse --is-shallow-repository)
            [ "-$isshallow" = "-false" ] && echo $possiblyexisting
        )
    done
}

# $1 is relative path of submodule
referenceargs=$(existingsubmodules $1 | sed -e 's;^;--reference=;')
extraargs="--recommend-shallow"
if [ "third_party/llvm-project" = "$1" ]; then
    extraargs="--depth=1"
fi
echo git submodule update --init $referenceargs --dissociate $extraargs -- $1
git submodule update --init $referenceargs --dissociate $extraargs -- $1
