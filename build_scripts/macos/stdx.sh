#!/bin/sh
set -e
. $(dirname $0)/init_env.sh

# STDX
cd $WORKSPACE/cangjie_stdx;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release --include=$WORKSPACE/cangjie_compiler/include
python3 build.py install;
export CANGJIE_STDX_PATH=$WORKSPACE/cangjie_stdx/target/darwin_${ARCH}_cjnative/static/stdx;

