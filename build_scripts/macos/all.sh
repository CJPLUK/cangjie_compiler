set -e
set -o xtrace
. $(dirname $0)/init_env.sh
cd $(dirname $0)

[ "$SKIP_COMPILER" -eq 1 ] || bash ./compiler.sh
[ "$SKIP_RUNTIME" -eq 1 ] || bash ./runtime.sh
[ "$SKIP_STDLIB" -eq 1 ] || bash ./stdlib.sh
[ "$SKIP_STDX" -eq 1 ] || bash ./stdx.sh

# tools
[ "$SKIP_CJPM" -eq 1 ] || bash ./cjpm.sh
[ "$SKIP_CJFMT" -eq 1 ] || bash ./cjfmt.sh
[ "$SKIP_HYPERLANG" -eq 1 ] || bash ./hyperlang.sh
[ "$SKIP_LSP" -eq 1 ] || bash ./lsp.sh

[ "$SKIP_BUNDLE" -eq 1 ] || bash ./bundleSDK.sh



