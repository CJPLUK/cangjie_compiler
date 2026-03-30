. $(dirname $0)/init_env.sh

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
