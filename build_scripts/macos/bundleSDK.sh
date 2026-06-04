set -o xtrace
. $(dirname $0)/init_env.sh

bundle_rsync() {
  local link_dest=$1
  shift

  if [ "$BUNDLE_LINK_DEST" -eq 1 ]; then
    rsync -a --link-dest="$link_dest" "$@"
  else
    rsync -a "$@"
  fi
}

# Bundle SDK
mkdir -p $WORKSPACE/software;
rm -rf $WORKSPACE/software/*;
cd $WORKSPACE/software;

# 拷贝cangjie目录
bundle_rsync "$WORKSPACE/cangjie_compiler/output" "$WORKSPACE/cangjie_compiler/output/" cangjie/;

# 删除 ast-support.a
rm -rf cangjie/lib/darwin_${ARCH}_cjnative/libcangjie-ast-support.a

# 组织文件
bundle_rsync "$WORKSPACE/cangjie_tools/cjpm/dist" "$WORKSPACE/cangjie_tools/cjpm/dist/cjpm" cangjie/tools/bin/;
mkdir -p cangjie/tools/config;
bundle_rsync "$WORKSPACE/cangjie_tools/cjfmt/build/build/bin" "$WORKSPACE/cangjie_tools/cjfmt/build/build/bin/cjfmt" cangjie/tools/bin/;
bundle_rsync "$WORKSPACE/cangjie_tools/cjfmt/config" "$WORKSPACE/cangjie_tools/cjfmt/config/"*.toml cangjie/tools/config/;
bundle_rsync "$WORKSPACE/cangjie_tools/hyperlangExtension/target/bin" "$WORKSPACE/cangjie_tools/hyperlangExtension/target/bin/main" cangjie/tools/bin/;
mv cangjie/tools/bin/main cangjie/tools/bin/hle;
bundle_rsync "$WORKSPACE/cangjie_tools/hyperlangExtension/src" "$WORKSPACE/cangjie_tools/hyperlangExtension/src/dtsparser/" cangjie/tools/dtsparser/;
rm -rf cangjie/tools/dtsparser/*.cj;
bundle_rsync "$WORKSPACE/cangjie_tools/cangjie-language-server/output/bin" "$WORKSPACE/cangjie_tools/cangjie-language-server/output/bin/LSPServer" cangjie/tools/bin/;

# Then copy over stdx artifacts
bundle_rsync "$WORKSPACE/cangjie_stdx/target" "$WORKSPACE/cangjie_stdx/target/" cangjie/stdx/
echo 'export CANGJIE_STDX_PATH=${CANGJIE_HOME}/stdx' >> cangjie/envsetup.sh
echo 'export LD_LIBRARY_PATH=${CANGJIE_HOME}/darwin_${ARCH}_cjnative/dynamic/stdx:${LD_LIBRARY_PATH}' >> cangjie/envsetup.sh
bundle_rsync "$PWD/cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx" cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx/*.dylib cangjie/runtime/lib/darwin_${ARCH}_cjnative/
bundle_rsync "$PWD/cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx" cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx/libstdx.syntaxFFI.a cangjie/runtime/lib/darwin_${ARCH}_cjnative/; # Maybe this should go here? libstdx.syntaxFFI.a is the only .a in that directory...
bundle_rsync "$PWD/cangjie/stdx/darwin_${ARCH}_cjnative/static/stdx" cangjie/stdx/darwin_${ARCH}_cjnative/static/stdx/*.a cangjie/lib/darwin_${ARCH}_cjnative/
mkdir cangjie/modules/darwin_${ARCH}_cjnative/stdx/;
bundle_rsync "$PWD/cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx" cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx/*.cjo cangjie/modules/darwin_${ARCH}_cjnative/stdx/;
bundle_rsync "$PWD/cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx" cangjie/stdx/darwin_${ARCH}_cjnative/dynamic/stdx/stdx.cjo cangjie/modules/darwin_${ARCH}_cjnative/;

# 打包和设置权限
chmod -R 750 cangjie
gtar --format=gnu -zcvf cangjie-sdk-${SDK_NAME}-${CANGJIE_VERSION}.tar.gz cangjie;
chmod 550 cangjie-sdk-${SDK_NAME}-${CANGJIE_VERSION}.tar.gz;
