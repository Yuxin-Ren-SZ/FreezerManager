#!/bin/bash
cd "$(dirname "$0")"
export QT_PLUGIN_PATH=/home/yuxin/.conan2/p/b/qt09ccbf1ceb1b2/p/plugins
export LD_LIBRARY_PATH=/home/yuxin/.conan2/p/b/qt09ccbf1ceb1b2/p/lib:$LD_LIBRARY_PATH
export QT_QPA_PLATFORM=xcb
export FONTCONFIG_PATH=/home/yuxin/.conan2/p/b/fontc6e1fff789d08f/p/res/etc/fonts
exec out/build/dev/src/qt/freezermanager_qt "$@"
