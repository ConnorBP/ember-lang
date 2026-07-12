#!/usr/bin/env bash
# Build, smoke-test, and package the standalone Ember distribution.
# Usage: scripts/package_release.sh [build-dir] [version]
# Produces dist/ember-<version>-<platform>.zip (or .tar.gz if zip is absent).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-buildt}"
VERSION="${2:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}"
[[ "$BUILD" = /* ]] || BUILD="$ROOT/$BUILD"
PLATFORM="$(uname -s | tr '[:upper:]' '[:lower:]' | tr -cd '[:alnum:]_-')-$(uname -m)"
STAGE="$ROOT/dist/ember-$VERSION-$PLATFORM"
SMOKE="$BUILD/package-smoke"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *) EXE= ;;
esac

printf '[1/4] Configuring and building release tools\n'
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j 8 --target ember_cli ember_bundle ember_stub_main

for name in ember_cli ember_bundle ember_stub_main; do
  [[ -f "$BUILD/$name$EXE" ]] || { echo "missing artifact: $BUILD/$name$EXE" >&2; exit 1; }
done

printf '[2/4] Smoke-testing ember bundle\n'
rm -rf "$SMOKE"
mkdir -p "$SMOKE"
printf 'fn main() -> i64 { return 42; }\n' > "$SMOKE/smoke.ember"
"$BUILD/ember_cli$EXE" bundle "$SMOKE/smoke.ember" "$SMOKE/smoke$EXE" \
  --stub "$BUILD/ember_stub_main$EXE"
set +e
"$SMOKE/smoke$EXE"
SMOKE_RC=$?
set -e
[[ $SMOKE_RC -eq 42 ]] || { echo "bundle smoke test returned $SMOKE_RC, expected 42" >&2; exit 1; }

printf '[3/4] Staging binaries, docs, and examples\n'
rm -rf "$STAGE"
mkdir -p "$STAGE/bin"
cp "$BUILD/ember_cli$EXE" "$BUILD/ember_bundle$EXE" "$BUILD/ember_stub_main$EXE" "$STAGE/bin/"
cp -R "$ROOT/docs" "$ROOT/examples" "$STAGE/"
cp "$ROOT/README.md" "$ROOT/LICENSE" "$STAGE/"
cat > "$STAGE/BUNDLING.txt" <<'DOC'
Standalone bundles
==================
Keep ember_cli and ember_stub_main in the same bin directory, then run:
  ember_cli bundle input.ember output.exe

The default source capability policy is `--permissions none`, which rejects
FFI/OS-I/O natives. Trusted applications may opt in with
`--permissions ffi`. The output replaces an existing file. By default it receives the runtime stub's
filesystem mode; `--output-permissions preserve` retains an existing output's
mode. Windows ACL inheritance remains OS-managed.
Use `--stub PATH` to select another compatible runtime stub and `--fn NAME`
to bake a non-main entrypoint into the executable.
DOC

printf '[4/4] Creating archive\n'
mkdir -p "$ROOT/dist"
if command -v zip >/dev/null 2>&1; then
  ARCHIVE="$STAGE.zip"
  rm -f "$ARCHIVE"
  (cd "$ROOT/dist" && zip -qr "$(basename "$ARCHIVE")" "$(basename "$STAGE")")
else
  ARCHIVE="$STAGE.tar.gz"
  rm -f "$ARCHIVE"
  tar -C "$ROOT/dist" -czf "$ARCHIVE" "$(basename "$STAGE")"
fi
printf 'Packaged: %s\n' "$ARCHIVE"
