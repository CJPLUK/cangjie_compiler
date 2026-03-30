#!/bin/sh
set -e
. $(dirname $0)/init_env.sh

# LSP
cd $WORKSPACE/cangjie_tools/cangjie-language-server/build;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release;
python3 build.py install;

