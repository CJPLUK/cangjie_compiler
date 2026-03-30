#!/bin/sh
set -e
. $(dirname $0)/init_env.sh

# STD
cd $WORKSPACE/cangjie_runtime/stdlib;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release --target-lib=$WORKSPACE/cangjie_runtime/runtime/output
python3 build.py install;
cp -R $WORKSPACE/cangjie_runtime/stdlib/output/* $WORKSPACE/cangjie_compiler/output/;

