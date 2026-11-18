#!/usr/bin/env node
// c-components.mjs — CLI over scripts/lib/components.mjs for code no package manager declares.
//   node scripts/c-components.mjs osv-input <root> <out.json> [components.json]
//   node scripts/c-components.mjs label <osv.json> <root> [components.json]
//   node scripts/c-components.mjs merge-cdx <cdx.json> <root> [components.json]
//   node scripts/c-components.mjs check <root> [components.json]
// The list defaults to <root>/security/components.json, then to this checkout's copy.

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { DEFAULT_FILE, loadInventory, toOsvInput, labelOsv, mergeIntoCdx } from './lib/components.mjs';

const SELF_FILE = join(dirname(fileURLToPath(import.meta.url)), '..', DEFAULT_FILE);
const [cmd, ...args] = process.argv.slice(2);

const fail = (msg) => { console.error(`::error::c-components: ${msg}`); process.exit(1); };

function inventory(root, file) {
  const list = file || (existsSync(join(root, DEFAULT_FILE)) ? join(root, DEFAULT_FILE) : SELF_FILE);
  let inv;
  try { inv = loadInventory(root, list); } catch (e) { fail(e.message); }
  for (const w of inv.warnings) console.error(`::warning::${w}`);
  return inv;
}

const readJson = (p) => { try { return JSON.parse(readFileSync(p, 'utf8')); } catch (e) { return fail(`cannot read "${p}" (${e.code || e.message})`); } };

switch (cmd) {
  case 'osv-input': {
    const [root, out, file] = args;
    if (!root || !out) fail('usage: osv-input <root> <out.json> [components.json]');
    const doc = toOsvInput(inventory(root, file));
    writeFileSync(out, JSON.stringify(doc, null, 2));
    console.log(`Wrote ${out}: ${doc.results[0].packages.length} git commit(s) to scan.`);
    break;
  }
  case 'label': {
    const [osvPath, root, file] = args;
    if (!osvPath || !root) fail('usage: label <osv.json> <root> [components.json]');
    const osv = readJson(osvPath);
    if (!Array.isArray(osv.results)) fail(`"${osvPath}" has no results array`);
    const n = labelOsv(osv, inventory(root, file));
    writeFileSync(osvPath, JSON.stringify(osv, null, 2));
    console.log(`Labelled ${n} git package(s) in ${osvPath}.`);
    break;
  }
  case 'merge-cdx': {
    const [cdxPath, root, file] = args;
    if (!cdxPath || !root) fail('usage: merge-cdx <cdx.json> <root> [components.json]');
    const cdx = readJson(cdxPath);
    const r = mergeIntoCdx(cdx, inventory(root, file), root);
    writeFileSync(cdxPath, JSON.stringify(cdx, null, 2));
    console.log(`Merged ${r.added} inventory component(s) into ${cdxPath} (dropped ${r.dropped} cdxgen detection(s) replaced by it or include-level, kept ${r.kept}).`);
    break;
  }
  case 'check': {
    const [root, file] = args;
    if (!root) fail('usage: check <root> [components.json]');
    const inv = inventory(root, file);
    const problems = [...inv.warnings];
    for (const c of inv.components) {
      if (c.licenseFile && !existsSync(join(root, c.licenseFile))) problems.push(`${c.name}: licence file "${c.licenseFile}" not found`);
    }
    if (problems.length) fail(problems.join('; '));
    console.log(`${inv.components.length} component(s) match the tree.`);
    break;
  }
  default:
    fail('usage: c-components.mjs osv-input | label | merge-cdx | check');
}
