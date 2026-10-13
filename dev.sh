#!/data/data/com.termux/files/usr/bin/bash
# see ../raylib-android/kit.sh
exec "$(dirname "$0")/../raylib-android/kit.sh" dev "$(dirname "$0")" "$@"
