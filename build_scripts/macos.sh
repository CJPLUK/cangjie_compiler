#!/bin/sh
set -e

SKIP_CLEAN=0
while [ $# -gt 0 ]; do
  case "$1" in
    --skip-clean) SKIP_CLEAN=1; shift ;;
    *) shift ;;
  esac
done

: "${CANGJIE_VERSION:=unofficial}"
: "${SDK_NAME:=stdx}"
WORKSPACE=$(realpath $(dirname $0)/..)
echo $WORKSPACE

# Cangjie Compiler
#
cd $WORKSPACE/cangjie_compiler;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
[ -d third_party/llvm-project ] || git clone ../third_party/llvm-project/ third_party/llvm-project
[ -d third_party/flatbuffers ] || git clone ../third_party/third_party_flatbuffers third_party/flatbuffers
python3 build.py build -t release --no-tests;
python3 build.py install;

source output/envsetup.sh
cjc -v

# Runtime
cd $WORKSPACE/cangjie_runtime/runtime;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release -v ${CANGJIE_VERSION};
python3 build.py install;
cp -R $WORKSPACE/cangjie_runtime/runtime/output/common/darwin_release_${ARCH}/{lib,runtime} $WORKSPACE/cangjie_compiler/output;

# STD
cd $WORKSPACE/cangjie_runtime/stdlib;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release --target-lib=$WORKSPACE/cangjie_runtime/runtime/output
python3 build.py install;
cp -R $WORKSPACE/cangjie_runtime/stdlib/output/* $WORKSPACE/cangjie_compiler/output/;

# STDX
cd $WORKSPACE/cangjie_stdx;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release --include=$WORKSPACE/cangjie_compiler/include
python3 build.py install;
export CANGJIE_STDX_PATH=$WORKSPACE/cangjie_stdx/target/darwin_${ARCH}_cjnative/static/stdx;

# tools
#
# cjpm
cd $WORKSPACE/cangjie_tools/cjpm/build;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release --set-rpath @loader_path/../../runtime/lib/darwin_${ARCH}_cjnative;
python3 build.py install;
# cjfmt
cd $WORKSPACE/cangjie_tools/cjfmt/build;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release;
python3 build.py install;
# hyperlang
cd $WORKSPACE/cangjie_tools/hyperlangExtension/build;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release;
python3 build.py install;
# LSP
cd $WORKSPACE/cangjie_tools/cangjie-language-server/build;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release;
python3 build.py install;


# Bundle SDK
mkdir -p $WORKSPACE/software;
rm -rf $WORKSPACE/software/*;
cd $WORKSPACE/software;

# 拷贝cangjie目录
cp -R $WORKSPACE/cangjie_compiler/output cangjie;

# 删除 ast-support.a
rm -rf cangjie/lib/darwin_${ARCH}_cjnative/libcangjie-ast-support.a

# 组织文件
cp $WORKSPACE/cangjie_tools/cjpm/dist/cjpm cangjie/tools/bin/cjpm;
mkdir -p cangjie/tools/config;
cp $WORKSPACE/cangjie_tools/cjfmt/build/build/bin/cjfmt cangjie/tools/bin;
cp $WORKSPACE/cangjie_tools/cjfmt/config/*.toml cangjie/tools/config;
cp $WORKSPACE/cangjie_tools/hyperlangExtension/target/bin/main cangjie/tools/bin/hle;
cp -r $WORKSPACE/cangjie_tools/hyperlangExtension/src/dtsparser cangjie/tools;
rm -rf cangjie/tools/dtsparser/*.cj;
cp $WORKSPACE/cangjie_tools/cangjie-language-server/output/bin/LSPServer cangjie/tools/bin;

# Then copy over stdx artifacts
cp -r $WORKSPACE/cangjie_stdx/target cangjie/stdx
echo 'export CANGJIE_STDX_PATH=${CANGJIE_HOME}/stdx' >> cangjie/envsetup.sh
echo 'export LD_LIBRARY_PATH=${CANGJIE_HOME}/darwin_${ARCH}_cjnative/dynamic/stdx:${LD_LIBRARY_PATH}' >> cangjie/envsetup.sh
cp cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx/*.dylib cangjie/runtime/lib/darwin_${ARCH}_cjnative/
cp cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx/libstdx.syntaxFFI.a cangjie/runtime/lib/darwin_${ARCH}_cjnative/; # Maybe this should go here? libstdx.syntaxFFI.a is the only .a in that directory...
cp cangjie/stdx/darwin_${ARCH}_cjnative/static/stdx/*.a cangjie/lib/darwin_${ARCH}_cjnative/
mkdir cangjie/modules/darwin_${ARCH}_cjnative/stdx/;
cp cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx/*.cjo cangjie/modules/darwin_${ARCH}_cjnative/stdx/;
cp cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx/stdx.cjo cangjie/modules/darwin_${ARCH}_cjnative/;

# 打包和设置权限
chmod -R 750 cangjie
gtar --format=gnu -zcvf cangjie-sdk-${SDK_NAME}-${CANGJIE_VERSION}.tar.gz cangjie;
chmod 550 cangjie-sdk-${SDK_NAME}-${CANGJIE_VERSION}.tar.gz;
