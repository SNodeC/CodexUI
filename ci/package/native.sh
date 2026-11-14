#!/bin/sh
set -eu
family=$(printf '%s' "$TARGET_JSON" | jq -r .family)
build=/tmp/codex/build/codexui/wave
mkdir -m 700 -p "$build/r" "$OUTPUT_DIR"
export TMPDIR="$build" XDG_RUNTIME_DIR="$build/r" QT_QPA_PLATFORM=offscreen CODEXUI_TIMING_POLICY=report
cmake -S "$SOURCE_DIR" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON -DCODEXUI_INSTALL_WEB=OFF
cmake --build "$build" --parallel "$(nproc)"
xvfb-run -a ctest --test-dir "$build" -LE mariadb --output-on-failure --verbose
case "$family" in
    deb) set -- -G DEB -D "CPACK_PACKAGE_VERSION=$PACKAGE_VERSION-$PACKAGE_RELEASE" ;;
    rpm) set -- -G RPM -D "CPACK_PACKAGE_VERSION=$PACKAGE_VERSION" \
                -D "CPACK_RPM_PACKAGE_RELEASE=$PACKAGE_RELEASE" ;;
esac
cpack --config "$build/CPackConfig.cmake" -B "$build" -D "CPACK_OUTPUT_FILE_PREFIX=$OUTPUT_DIR" "$@"
