#!/usr/bin/env bash
#
# osv-scan.sh — osv-scanner over a source tree of this repository.
#
#   scripts/osv-scan.sh <root> <out.json> <config.toml>
#
# Scans the git commits declared in security/components.json (submodules read
# from the tree, vendored sources by their upstream commit) plus any lockfile
# under <root>. The config is required and passed as --config: without it,
# osv-scanner applies <root>/osv-scanner.toml. The raw scan passes the empty
# security/osv-raw.toml.
#
# The directory plugins (vendored, gitrepo) are disabled: on libs/unity they
# reported a different commit of another library (cJSON) on every run.
#
# Fails closed: any osv-scanner exit other than 0 (clean) or 1 (findings) is an
# error. An empty result is written as {"results":[]}, never as a missing file.

set -euo pipefail

usage="usage: osv-scan.sh <root> <out.json> <config.toml>"
root="${1:?$usage}"
out="${2:?$usage}"
cfg="${3:?$usage}"
[ -f "$cfg" ] || { echo "::error::osv-scan.sh: config \"$cfg\" not found"; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"

inv="$(mktemp -d)/osv-scanner.json"
node "$here/c-components.mjs" osv-input "$root" "$inv"

args=(scan --config="$cfg" --recursive --experimental-disable-plugins directory -L "osv-scanner:$inv" --format=json --output-file="$out")

set +e
osv-scanner "${args[@]}" "$root"
rc=$?
set -e
if [ "$rc" != "0" ] && [ "$rc" != "1" ]; then
  echo "::error::osv-scanner failed on $root (exit $rc)"; exit 1
fi
[ -s "$out" ] || echo '{"results":[]}' > "$out"
node "$here/c-components.mjs" label "$out" "$root"
