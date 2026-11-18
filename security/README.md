# Security — SBOM and vulnerability flows

Two separate flows, run by GitHub Actions:

| Flow | What it is | When it runs | Output |
|---|---|---|---|
| **SBOM** (`security-sbom.yml`) | The bill of materials: every dependency with its version and licence (CycloneDX 1.6, SPDX 2.3, components CSV) | Once per release: a commit landing on `main`. A released version's SBOM never changes, so it is not regenerated on a schedule | Branch `security-archive`, folder `pr-<n>-<date>/` |
| **Vulnerabilities, PR check** (`security-pr-gate.yml`) | osv-scanner on the PR head and base | Every PR into `development` or `main` | Blocks only a **critical advisory the PR introduces**; pre-existing advisories never block. Sticky PR comment, no SBOM, no report |
| **Vulnerabilities, release report** (`security-release-vulns.yml`) | Scan plus the vulnerability report (md, html, pdf) | Once per release, next to the SBOM | Same `security-archive` folder as the SBOM |
| **Vulnerabilities, monthly** (`security-monthly.yml`) | Scan, Claude triage of new advisories, vulnerability report | 1st of every month, 06:00 UTC, and on demand | A PR into `main` with `security/<YYYY-MM>/` |

The vulnerability flows scan the components in `components.json` by git commit
(`scripts/osv-scan.sh`) and honour the VEX baseline in `../osv-scanner.toml`;
they do not need the SBOM. A commit on `main` that only
touches `security/` and `osv-scanner.toml` (such as the merged monthly PR) is not
a release, so the release flows skip it.

```
security-archive (orphan branch)
└── pr-<n>-<date>/
    ├── <name>.cdx.json · <name>.spdx.json · <name>.components.csv · PROVENANCE-sbom.txt   ← SBOM flow
    └── vulnerabilities.csv · <Title>-Vulnerability-Report.md/.html/.pdf · PROVENANCE-vulns.txt ← vulnerability flow

security/ (this folder, on the code branches)
├── <YYYY-MM>/                       ← monthly vulnerability snapshot
├── LATEST                           ← text file naming the current month
├── components.json                  ← third-party code no package manager declares
└── report-config.json               ← report DATA (VEX triage + narrative)
```

(`LATEST` is a plain text file rather than a symlink: a committed symlink
materializes as a text file on Windows checkouts anyway, so it may as well be one
everywhere.)

> `on: schedule` only fires from the repository's **default branch**. To test the
> release flows without touching the real evidence, run them with **Run workflow**
> on any branch other than `main`; they write to `security-archive-test`.

## What is covered (C project, no package manager)

EtherDOG has no lockfile, so `components.json` is the inventory. `scripts/c-components.mjs`
merges it into the cdxgen output (`generate-sbom.sh`) and turns it into the
osv-scanner input (`osv-scan.sh`).

| Component | Where | In the SBOM | Scanned for advisories |
|---|---|---|---|
| SOEM | submodule `libs/soem` (built from a patched copy, `patches/soem`) | Yes, GPL-3.0-only, patches listed as pedigree | Yes, by the submodule commit in the tree |
| cJSON | vendored `src/cjson` | Yes, MIT | Yes, by the upstream commit of the declared version |
| Unity | submodule `libs/unity`, test binaries only | Yes, scope `excluded` | Yes, by the submodule commit |
| WinPcap Developer's Pack | bundled in SOEM, Windows builds only | Yes, BSD notices, version unknown | **No**: no upstream commit to match |
| Threads, Python3, Git | detected by cdxgen in the CMake files | Yes, as host/build tools, no licence asserted | **No** |
| Host C library, kernel, Npcap driver | provided by the host, not shipped | No | No |

Matching is by git commit against the commit ranges in OSV. An advisory that OSV
records only by version or CPE, with no commit range, is not found.

Keeping it current (the unit tests in the PR gate enforce this):

- **Submodule bump:** the scan always uses the commit in the tree, but the tests fail
  until `version` and `commit` in `components.json` are updated.
- **Vendored update:** update `version` and `commit` (the upstream commit of that
  release). If the header macros (`versionFrom`) disagree with `version`, every scan
  fails, since it would otherwise scan the old commit.
- **New third-party code:** add an entry. Without one it is in neither the SBOM nor
  the scan.

osv-scanner's `directory` plugins (vendored, gitrepo) are disabled in
`osv-scan.sh`: on `libs/unity` they reported a different commit of another
library (cJSON) on every run, which made the base/head comparison of the gate
unstable.

## What stops a bad report from shipping

The chain has to fail loudly rather than produce a clean-looking document from
nothing. Three guards do that:

- **`scripts/assert-sbom.mjs`** — a *content* floor on the generated SBOM (root plus
  the declared components). Every other stage validates only shape, so an empty
  CycloneDX file would travel the whole chain and come out as a report attesting a
  clean posture over zero components. Below the floor, the run stops.
- **Untriaged detection** — each entry in `affected[]` / `mitigated[]` /
  `notAffected[]` lists the advisory `ids` it accounts for. `build-report.mjs`
  compares those against the live post-suppression scan; anything unclaimed gets
  its own **section 4a — Untriaged in This Scan**, the executive summary is marked
  *Assessment incomplete*, and the stored posture statement is rendered with the
  date it was assessed. The document is allowed to be incomplete; it is not
  allowed to be quietly self-contradictory.
- **`scripts/lib/severity.mjs` and `scripts/lib/cdx.mjs`** — one severity verdict
  and one component-collection walk, shared by the gate, the report, the SPDX
  converter and the CSV converter. Previously the report bucketed an
  unscored advisory MODERATE while the gate blocked it as HIGH, and the SPDX
  declared more packages than the CSV listed. The release SBOM flow asserts the
  three component counts agree before archiving.

## The suppression baseline (`../osv-scanner.toml`)

Expiry dates are **staggered on purpose** — see the header comment in that file.
The offset from the review date comes from the VEX justification and is then
shifted by severity (CRITICAL is pulled to the first review, LOW/MODERATE pushed
back), so no single month inherits the whole baseline for re-review.

Suppressions written by the automated triage step carry
`AI-TRIAGED [unreviewed]` in their `reason`. **The AI step has authority to write
security suppressions and the only control is human review of the PR diff** —
clearing that marker is the reviewer's acknowledgement that a person checked it.
A step after the triage re-parses both files (and refuses a net *loss* of
suppressions) before anything is rendered.

To re-baseline by hand from a triaged register:

```sh
node scripts/gen-osv-ignores.mjs sbom/vulnerabilities.csv 2027-01-01 >> osv-scanner.toml
```

It refuses to run without a per-row verdict column, and it will not suppress an
advisory that any row triages as requiring action.

## One-time setup — run Claude on your subscription (no API key)

Claude runs inside the workflow on **your Claude subscription** (Pro/Max), not a
pay-per-token API key:

1. On your machine: `claude setup-token` → complete the browser login → copy the
   token it prints (`CLAUDE_CODE_OAUTH_TOKEN`).
2. In the repo: **Settings → Secrets and variables → Actions → New repository
   secret** → name `CLAUDE_CODE_OAUTH_TOKEN`, value = the token.
3. Done. The monthly workflow will authenticate as you. (If the secret is absent,
   the workflow still runs and builds the report deterministically — it just
   skips the AI triage step.)

> To move billing off a personal account later (e.g. an org API key), replace the
> secret with `ANTHROPIC_API_KEY` and swap the one input in the workflow — nothing
> else changes.

## Reviewing the monthly PR

- **No new advisory:** confirm the summary, approve, merge (the snapshot is archived).
- **New advisory, not exploitable:** check Claude's justification in the
  `osv-scanner.toml` diff, drop the `AI-TRIAGED [unreviewed]` marker from the
  entries you have verified, then merge.
- **New advisory, exploitable:** bump the dependency (separate PR) and merge the
  report that documents it.
- **Report says *Assessment incomplete*:** section 4a lists advisories nothing
  accounted for. Each one needs either a dependency bump or a documented
  `not_affected` determination with its id recorded in `report-config.json`.
