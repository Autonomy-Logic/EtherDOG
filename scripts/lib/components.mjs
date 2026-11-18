// components.mjs — resolve security/components.json against a source tree and
// turn it into CycloneDX components and an osv-scanner input.

import { readFileSync, existsSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';

export const DEFAULT_FILE = 'security/components.json';

// Commit recorded for a submodule gitlink in the tree at HEAD (works without the submodule checked out).
export function gitlinkCommit(root, path) {
  let out;
  try {
    out = execFileSync('git', ['-C', root, 'ls-tree', 'HEAD', '--', path], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (e) {
    throw new Error(`cannot read the git tree of "${root}" (${e.message.split('\n')[0]})`);
  }
  const m = out.match(/^160000 commit ([0-9a-f]{40})\t/m);
  if (!m) throw new Error(`"${path}" is not a submodule in the tree of "${root}"`);
  return m[1];
}

// Version from C header macros, joined with dots (e.g. MAJOR.MINOR.PATCH).
export function headerVersion(root, versionFrom) {
  const file = join(root, versionFrom.file);
  if (!existsSync(file)) throw new Error(`version file "${versionFrom.file}" not found in "${root}"`);
  const text = readFileSync(file, 'utf8');
  return versionFrom.macros.map((name) => {
    const m = text.match(new RegExp(`#define\\s+${name}\\s+(\\d+)`));
    if (!m) throw new Error(`macro ${name} not found in "${versionFrom.file}"`);
    return m[1];
  }).join('.');
}

// Resolves the declared list against the tree at <root>. Returns { product, components, warnings }.
// A vendored version that differs from the header throws: its declared commit would be scanned instead of the real code.
export function loadInventory(root, file) {
  const cfgPath = file || join(root, DEFAULT_FILE);
  const cfg = JSON.parse(readFileSync(cfgPath, 'utf8'));
  if (!cfg.product?.name || !Array.isArray(cfg.components)) throw new Error(`"${cfgPath}" needs product.name and components[]`);
  const warnings = [];
  const versionPath = cfg.product.versionFile ? join(root, cfg.product.versionFile) : null;
  const productVersion = versionPath && existsSync(versionPath) ? readFileSync(versionPath, 'utf8').split('\n')[0].trim() : '';
  const names = new Set();
  const components = cfg.components.map((c) => {
    if (!c.name || !c.kind) throw new Error(`component without name/kind in "${cfgPath}"`);
    const key = c.name.toLowerCase();
    if (names.has(key)) throw new Error(`duplicate component "${c.name}" in "${cfgPath}"`);
    names.add(key);
    const r = { ...c };
    if (c.kind === 'submodule') {
      const tree = gitlinkCommit(root, c.path);
      if (c.commit && c.commit !== tree) {
        warnings.push(`${c.name}: tree has ${tree.slice(0, 7)}, ${DEFAULT_FILE} declares ${c.commit.slice(0, 7)}; update its version and commit`);
        r.version = `git-${tree.slice(0, 7)}`;
      }
      r.commit = tree;
    } else if (c.kind === 'vendored' && c.versionFrom) {
      const found = headerVersion(root, c.versionFrom);
      if (found !== c.version) {
        throw new Error(`${c.name}: ${c.versionFrom.file} says ${found}, ${DEFAULT_FILE} declares ${c.version}; update its version and commit`);
      }
    }
    return r;
  });
  return { product: { ...cfg.product, version: productVersion }, components, warnings };
}

// osv-scanner custom lockfile (-L osv-scanner:<file>). The name must stay empty: OSV matches GIT commits only without one.
export function toOsvInput(inv) {
  const packages = inv.components.filter((c) => c.commit)
    .map((c) => ({ package: { name: '', version: '', ecosystem: 'GIT', commit: c.commit } }));
  return { results: [{ source: { path: DEFAULT_FILE, type: 'lockfile' }, packages }] };
}

// Names the GIT packages of an osv-scanner result after the inventory entry with the same commit.
export function labelOsv(osv, inv) {
  const byCommit = new Map(inv.components.filter((c) => c.commit).map((c) => [c.commit, c]));
  let n = 0;
  for (const r of (osv?.results || [])) {
    for (const p of (r.packages || [])) {
      const c = p.package?.commit && byCommit.get(p.package.commit);
      if (!c) continue;
      p.package.name = c.name;
      p.package.version = c.version || p.package.commit.slice(0, 7);
      n++;
    }
  }
  return n;
}

const licensesOf = (spdx) => {
  if (!spdx) return [{ license: { name: 'NOASSERTION' } }];
  return /\s/.test(spdx) ? [{ expression: spdx }] : [{ license: { id: spdx } }];
};

function githubPurl(repo, ref) {
  const m = String(repo || '').match(/^https:\/\/github\.com\/([^/]+)\/([^/.]+)/);
  return m && ref ? `pkg:github/${m[1].toLowerCase()}/${m[2].toLowerCase()}@${ref}` : null;
}

export function toCdxComponent(c, root) {
  const slug = c.name.toLowerCase().replace(/[^a-z0-9.-]+/g, '-');
  const purl = githubPurl(c.repo, c.commit) || `pkg:generic/${slug}${c.version ? `@${c.version}` : ''}`;
  const comp = {
    type: 'library',
    'bom-ref': purl,
    name: c.name,
    version: c.version || '',
    purl,
    scope: c.scope || 'required',
    licenses: licensesOf(c.license),
  };
  if (c.supplier) { comp.supplier = { name: c.supplier }; comp.publisher = c.supplier; }
  if (c.description) comp.description = c.description;
  if (c.repo) comp.externalReferences = [{ type: 'vcs', url: c.repo }];
  const props = [{ name: 'etherdog:kind', value: c.kind }];
  if (c.path) props.push({ name: 'etherdog:path', value: c.path });
  if (c.commit) props.push({ name: 'etherdog:commit', value: c.commit });
  if (c.licenseFile) props.push({ name: 'etherdog:licenseFile', value: c.licenseFile });
  comp.properties = props;
  if (c.patchesDir && root && existsSync(join(root, c.patchesDir))) {
    const patches = readdirSync(join(root, c.patchesDir)).filter((f) => f.endsWith('.patch')).sort();
    if (patches.length) comp.pedigree = { patches: patches.map((f) => ({ type: 'unofficial', diff: { url: `${c.patchesDir}/${f}` } })) };
  }
  return comp;
}

// A cdxgen detection from include-level source analysis: one per #include (system headers, first-party files), purl has a file subpath.
export const isIncludeDetection = (c) => /#/.test(String(c.purl || c['bom-ref'] || ''));

// Merges the inventory into a cdxgen BOM. Inventory entries replace cdxgen detections of the same name, include-level detections are dropped, the rest stay.
export function mergeIntoCdx(cdx, inv, root) {
  const p = inv.product;
  const oldRoot = cdx.metadata?.component?.['bom-ref'];
  const rootPurl = `${p.purl || `pkg:generic/${p.name}`}${p.version ? `@${p.version}` : ''}`;
  cdx.metadata = cdx.metadata || {};
  cdx.metadata.component = {
    ...(cdx.metadata.component || {}),
    type: 'application', name: p.name, version: p.version || '', purl: rootPurl, 'bom-ref': rootPurl,
    licenses: licensesOf(p.license),
    ...(p.supplier ? { supplier: { name: p.supplier }, publisher: p.supplier } : {}),
    ...(p.description ? { description: p.description } : {}),
  };
  const replaced = new Set([p.name.toLowerCase(), ...inv.components.map((c) => c.name.toLowerCase())]);
  const dropped = new Set();
  const kept = (cdx.components || []).filter((c) => {
    if (replaced.has(String(c.name).toLowerCase()) || isIncludeDetection(c)) { dropped.add(c['bom-ref']); return false; }
    return true;
  });
  const added = inv.components.map((c) => toCdxComponent(c, root));
  cdx.components = [...kept, ...added];

  const remap = (ref) => (ref === oldRoot ? rootPurl : ref);
  const deps = (cdx.dependencies || [])
    .filter((d) => !dropped.has(d.ref) || d.ref === oldRoot)
    .map((d) => ({ ref: remap(d.ref), dependsOn: (d.dependsOn || []).filter((r) => !dropped.has(r) || r === oldRoot).map(remap) }));
  let rootDep = deps.find((d) => d.ref === rootPurl);
  if (!rootDep) { rootDep = { ref: rootPurl, dependsOn: [] }; deps.unshift(rootDep); }
  for (const c of added) {
    if (c.scope !== 'excluded' && !rootDep.dependsOn.includes(c['bom-ref'])) rootDep.dependsOn.push(c['bom-ref']);
    if (!deps.some((d) => d.ref === c['bom-ref'])) deps.push({ ref: c['bom-ref'], dependsOn: [] });
  }
  cdx.dependencies = deps;
  return { added: added.length, dropped: dropped.size, kept: kept.length };
}
