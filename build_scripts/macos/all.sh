set -e
. $(dirname $0)/init_env.sh
MACOS_BUILD_SCRIPTS=${WORKSPACE}/build_scripts/macos


. ${MACOS_BUILD_SCRIPTS}/compiler.sh
. ${MACOS_BUILD_SCRIPTS}/runtime.sh
. ${MACOS_BUILD_SCRIPTS}/stdlib.sh
. ${MACOS_BUILD_SCRIPTS}/stdx.sh

# tools
. ${MACOS_BUILD_SCRIPTS}/cjpm.sh
. ${MACOS_BUILD_SCRIPTS}/cjfmt.sh
. ${MACOS_BUILD_SCRIPTS}/hyperlang.sh
. ${MACOS_BUILD_SCRIPTS}/lsp.sh

. ${MACOS_BUILD_SCRIPTS}/bundleSDK.sh



