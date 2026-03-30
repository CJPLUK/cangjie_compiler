#!/bin/sh
set -e
. $(dirname $0)/init_env.sh

# Cangjie Compiler
#
cd $WORKSPACE/cangjie_compiler;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
[ -d third_party/llvm-project ] || git clone ../third_party/llvm-project/ third_party/llvm-project
[ -d third_party/flatbuffers ] || git clone ../third_party/third_party_flatbuffers third_party/flatbuffers
python3 build.py build -t release --no-tests;
python3 build.py install;

. output/envsetup.sh
cjc -v

