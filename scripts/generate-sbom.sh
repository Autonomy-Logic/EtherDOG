#!/usr/bin/env bash
#
# generate-sbom.sh — Software Bill of Materials for this C project.
#
# Produces three artifacts under ./sbom/:
#   - <name>.cdx.json        CycloneDX 1.6
#   - <name>.spdx.json       SPDX 2.3
#   - <name>.components.csv  flattened component list with licences
#
# There is no package manager: cdxgen (-t c) reads the CMake files, and
# security/components.json supplies the submodules, vendored sources and bundled
# binaries, merged in by scripts/c-components.mjs.
#
# Usage:  ./scripts/generate-sbom.sh   (needs node and the submodules checked out)

set -euo pipefail
cd "$(dirname "$0")/.."

NAME="$(node -p "require('./security/components.json').product.name")"
VERSION="$(head -1 VERSION 2>/dev/null || echo "")"

OUT="sbom"
mkdir -p "$OUT"

echo "==> Checking security/components.json against the tree"
node scripts/c-components.mjs check .

# Pinned so the SBOM is reproducible. libs/ is covered by components.json; cdxgen
# would otherwise report the CMake targets of SOEM's unused ports as dependencies.
echo "==> Generating CycloneDX 1.6 SBOM (cdxgen, -t c) for ${NAME}@${VERSION:-unversioned}"
FETCH_LICENSE=true npx --yes @cyclonedx/cdxgen@11.11.0 \
  -t c \
  --spec-version 1.6 \
  --exclude "libs/**" --exclude "build*/**" --exclude "sbom/**" \
  -o "$OUT/$NAME.cdx.json" \
  --project-name "$NAME" \
  ${VERSION:+--project-version "$VERSION"} \
  .

echo "==> Merging security/components.json"
node scripts/c-components.mjs merge-cdx "$OUT/$NAME.cdx.json" .

# Content floor: root + the declared components. An empty BOM must fail here.
echo "==> Validating the SBOM has real content"
node scripts/assert-sbom.mjs "$OUT/$NAME.cdx.json" "${MIN_COMPONENTS:-5}"

echo "==> Converting to SPDX 2.3"
node scripts/cdx-to-spdx.mjs "$OUT/$NAME.cdx.json" "$OUT/$NAME.spdx.json"

echo "==> Emitting flattened CSV"
node scripts/cdx-to-csv.mjs "$OUT/$NAME.cdx.json" "$OUT/$NAME.components.csv"

echo "==> Done. Artifacts in ./$OUT/"
ls -la "$OUT"
