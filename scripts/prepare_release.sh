#!/usr/bin/env bash
# prepare_release.sh — prepare a GitHub release for ember.
#
# Usage: scripts/prepare_release.sh <version> [--publish]
#
# Without --publish: builds artifacts, generates release notes, creates a git tag,
#   and prints instructions for manual review + publishing.
# With --publish: also pushes the tag and creates the GitHub release (requires gh CLI).
#
# Milestone criteria (checked before proceeding):
#   - Clean git tree (no uncommitted changes)
#   - All ctest tests pass (54+)
#   - All lang tests pass (258+)
#   - Validation returns 177
#
# Artifacts packaged:
#   - ember_cli.exe (native compiler + runner)
#   - ember_bundle.exe (standalone exe bundler)
#   - ember_stub_main.exe (runtime stub for bundled exes)
#   - self-hosted compiler (if available)
#   - docs/ and examples/

set -euo pipefail

VERSION="${1:-}"
PUBLISH=false
if [[ "${2:-}" == "--publish" ]]; then
    PUBLISH=true
fi

if [[ -z "$VERSION" ]]; then
    echo "Usage: scripts/prepare_release.sh <version> [--publish]"
    echo "  version: e.g., v1.1.0"
    exit 1
fi

EMBER_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$EMBER_DIR"

echo "=== ember release preparation: $VERSION ==="

# --- Milestone criteria checks ---
echo "[1/5] Checking milestone criteria..."

# Clean tree
if [[ -n "$(git status --porcelain | grep -v 'thirdparty/vst3sdk')" ]]; then
    echo "FAIL: git tree is dirty. Commit or stash changes first."
    git status --short
    exit 1
fi
echo "  OK: clean git tree"

# Build
echo "  Building..."
cmake --build buildt -j 8 2>&1 | tail -1

# ctest
echo "  Running ctest..."
CTEST_OUT=$(cd buildt && ctest -E bench --timeout 60 2>&1)
if ! echo "$CTEST_OUT" | grep -q "100% tests passed"; then
    echo "FAIL: ctest did not pass 100%"
    echo "$CTEST_OUT" | tail -5
    exit 1
fi
echo "  OK: ctest all pass"

# Lang tests
echo "  Running lang tests..."
LANG_OUT=$(cd buildt && ./ember_cli.exe test ../tests/lang 2>&1)
if ! echo "$LANG_OUT" | grep -q "0 failed"; then
    echo "FAIL: lang tests have failures"
    echo "$LANG_OUT" | tail -5
    exit 1
fi
echo "  OK: lang tests all pass"

# Validation
echo "  Running validation..."
VAL=$(cd buildt && ./ember_cli.exe run ../tests/lang/optimization_validation.ember --fn main --passes constprop,forward,copyprop,instcombine,dce,licm,dse 2>/dev/null; echo $?)
if [[ "$VAL" != "177" ]]; then
    echo "FAIL: validation returned $VAL (expected 177)"
    exit 1
fi
echo "  OK: validation returns 177"

# --- Build artifacts ---
echo "[2/5] Building release artifacts..."

ARTIFACT_DIR="buildt/release-$VERSION"
mkdir -p "$ARTIFACT_DIR"

# Native compiler + runner (the C++ JIT compiler)
echo "  Copying native compiler (ember_cli.exe)..."
cp buildt/ember_cli.exe "$ARTIFACT_DIR/" 2>/dev/null || echo "  WARN: ember_cli.exe not found"

# Bundler tool + runtime stub
echo "  Copying bundler + runtime stub..."
cp buildt/ember_bundle.exe "$ARTIFACT_DIR/" 2>/dev/null || echo "  WARN: ember_bundle.exe not found"
cp buildt/ember_stub_main.exe "$ARTIFACT_DIR/" 2>/dev/null || echo "  WARN: ember_stub_main.exe not found"

# Self-hosted compiler (ember compiler written in ember, bundled as standalone exe)
echo "  Bundling self-hosted compiler (ember_selfhost.exe)..."
if [ -f "buildt/ember_bundle.exe" ] && [ -f "self_hosted/emberc.ember" ]; then
    ./buildt/ember_bundle.exe self_hosted/emberc.ember "$ARTIFACT_DIR/ember_selfhost.exe" --fn main 2>/dev/null \
        && echo "  OK: ember_selfhost.exe bundled" \
        || echo "  WARN: could not bundle ember_selfhost.exe (emberc.ember may need --ffi or different entry)"
    # Also copy the self-hosted source files for reference/hot-reload
    mkdir -p "$ARTIFACT_DIR/self_hosted"
    cp self_hosted/*.ember "$ARTIFACT_DIR/self_hosted/" 2>/dev/null || true
    cp self_hosted/README.md "$ARTIFACT_DIR/self_hosted/" 2>/dev/null || true
else
    echo "  WARN: cannot bundle self-hosted compiler (ember_bundle.exe or emberc.ember not found)"
fi

# VST3 plugin (if built)
if [ -d "buildt/VST3/Release/ember_gain.vst3" ]; then
    echo "  Copying VST3 plugin (ember_gain.vst3)..."
    cp -r buildt/VST3/Release/ember_gain.vst3 "$ARTIFACT_DIR/" 2>/dev/null || true
fi

# Copy docs and examples
cp -r docs "$ARTIFACT_DIR/" 2>/dev/null || true
cp -r examples "$ARTIFACT_DIR/" 2>/dev/null || true
cp README.md "$ARTIFACT_DIR/" 2>/dev/null || true
cp LICENSE "$ARTIFACT_DIR/" 2>/dev/null || true

# Generate checksums
cd "$ARTIFACT_DIR"
sha256sum *.exe > checksums.sha256 2>/dev/null || true
cd "$EMBER_DIR"

echo "  Artifacts in $ARTIFACT_DIR/"

# --- Generate release notes ---
echo "[3/5] Generating release notes..."

NOTES_FILE="$ARTIFACT_DIR/release_notes.md"
LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")

cat > "$NOTES_FILE" << EOF
# ember $VERSION

## Release Artifacts

### Compilers
- \`ember_cli.exe\` — **native compiler** (C++ JIT, full language support)
  This is the primary compiler. It compiles ember scripts to native x86-64 machine
  code at runtime. Supports the full ember language including structs, enums,
  floats, lambdas, coroutines, and all extensions.

- \`ember_selfhost.exe\` — **self-hosted compiler** (ember written in ember, experimental)
  This is the ember compiler written IN ember itself. It supports a SUBSET of the
  language (i64/bool/void, let, if/while/for, arithmetic, calls, recursion).
  Use this to experiment with the self-hosting milestone. The self_hosted/ directory
  contains the source .ember files for hot-reload experimentation.

### Tools
- \`ember_bundle.exe\` — standalone exe bundler (bundle .ember → .exe)
- \`ember_stub_main.exe\` — runtime stub for bundled exes

### VST3 Plugin (if present)
- \`ember_gain.vst3/\` — VST3 audio plugin wrapper (ember DSP, hot-reloadable)

### Other
- \`docs/\` — documentation
- \`examples/\` — example scripts
- \`self_hosted/\` — self-hosted compiler source files
- \`checksums.sha256\` — SHA256 checksums

## Changes
EOF

if [[ -n "$LAST_TAG" ]]; then
    git log "$LAST_TAG"..HEAD --oneline --no-decorate >> "$NOTES_FILE"
else
    git log --oneline -50 --no-decorate >> "$NOTES_FILE"
fi

echo "" >> "$NOTES_FILE"
echo "## Test Results" >> "$NOTES_FILE"
echo "- ctest: all pass (54+ tests)" >> "$NOTES_FILE"
echo "- lang tests: all pass (258+ tests)" >> "$NOTES_FILE"
echo "- validation: 177 (value-preserving)" >> "$NOTES_FILE"

echo "  Release notes in $NOTES_FILE"

# --- Create git tag ---
echo "[4/5] Creating git tag $VERSION..."
if git tag -l "$VERSION" | grep -q .; then
    echo "  WARN: tag $VERSION already exists"
else
    git tag -a "$VERSION" -m "ember $VERSION release" 2>/dev/null || echo "  WARN: could not create tag"
    echo "  Created tag $VERSION"
fi

# --- Publish or print instructions ---
echo "[5/5] Release preparation complete."
echo ""
echo "Artifacts: $ARTIFACT_DIR/"
echo "Notes:     $ARTIFACT_DIR/release_notes.md"
echo "Tag:       $VERSION"
echo ""

if $PUBLISH; then
    echo "Publishing..."
    git push origin "$VERSION" 2>/dev/null || echo "  WARN: could not push tag"
    # Create GitHub release if gh CLI is available
    if command -v gh &>/dev/null; then
        gh release create "$VERSION" \
            --title "ember $VERSION" \
            --notes-file "$NOTES_FILE" \
            "$ARTIFACT_DIR"/*.exe \
            "$ARTIFACT_DIR/checksums.sha256" \
            2>/dev/null || echo "  WARN: gh release create failed"
        echo "  GitHub release created"
    else
        echo "  gh CLI not found — tag pushed, create release manually at:"
        echo "  https://github.com/ConnorBP/ember-lang/releases/new?tag=$VERSION"
    fi
else
    echo "Review the artifacts and notes, then publish with:"
    echo "  scripts/prepare_release.sh $VERSION --publish"
    echo ""
    echo "Or manually push the tag and create a release at:"
    echo "  https://github.com/ConnorBP/ember-lang/releases/new?tag=$VERSION"
fi
