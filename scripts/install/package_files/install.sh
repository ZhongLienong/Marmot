#!/bin/sh
# Puts this folder's Marmot on PATH and its prelude on MARMOT_PATH: writes env.sh
# beside this script, for the shell profile to source. Run it again after moving
# the folder.
set -eu
root=$(cd "$(dirname "$0")" && pwd)
cat > "$root/env.sh" <<EOF
export MARMOT_PATH="$root/MarmotPrelude\${MARMOT_PATH:+:\$MARMOT_PATH}"
export PATH="$root/bin:\$PATH"
EOF
echo "Add this line to your shell profile (e.g. ~/.profile), then open a new terminal:"
echo "  . \"$root/env.sh\""
