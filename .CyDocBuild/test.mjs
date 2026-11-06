import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { createLink, parseLink } from 'cydoc';
import { verifyManual } from './verify.mjs';
const output = fileURLToPath(new URL('../Documentation/', import.meta.url));
const { docs, manifest, report } = await verifyManual(output);
test('English and French are complete, tracked and are the only offered languages', () => {
  assert.deepEqual(manifest.locales.map(x => x.code), ['en', 'fr']);
  assert.equal(report.missing.length + report.outdated.length + report.untracked.length, 0);
  for (const page of manifest.pages) { assert.equal(docs.resolve(page.id, page.locale).fallback, false); assert.ok(page.html.trim()); }
});
test('stable page, section and media links survive local route round trips', () => {
  const route = { locale: 'fr', pageId: 'black-white', anchor: 'compare', state: { media: 'color-comparison', wipe: '35' } };
  const link = createLink(route, 'file:///C:/Apps/CyImgView/Documentation/Documentation.html');
  assert.deepEqual(parseLink(link.slice(link.indexOf('#'))), route);
  assert.match(docs.resolve('black-white', 'fr').page.html, /data-cydoc-id="color-comparison"/);
});
test('legacy page addresses resolve to their current human guides', () => {
  for (const [id, replacement] of [['mode-edition', 'edit'], ['write-a-plugin', 'decoder-api'], ['source-api-include-cyimg-plugin-h', 'decoder-api'], ['translation-report', 'settings'], ['compiler', 'plugins']])
    assert.equal(docs.canonicalRoute({ locale: 'fr', pageId: id }).pageId, replacement);
});
test('real guide and API parents form a readable tree', () => {
  assert.equal(docs.resolve('layers', 'en').page.parent, 'edit');
  assert.equal(docs.resolve('new-document', 'fr').page.parent, 'edit');
  const api = manifest.pages.filter(page => page.kind === 'api' && page.locale === 'en');
  assert.equal(api.length, 18);
  assert.ok(api.every(page => ['decoder-api', 'editor-api'].includes(page.parent)));
});
test('structured API import documents signatures, parameters, returns and relationships without source listings', () => {
  const page = docs.resolve('api-cyimghost-alloc-bgra-alloc-bgra', 'fr').page;
  assert.match(page.html, /uint8_t/); assert.match(page.html, /Paramètres/); assert.match(page.html, /Retour/);
  assert.match(page.html, /Largeur de sortie en pixels/);
  assert.equal(manifest.pages.some(page => page.kind === 'source' || page.sourcePageId), false);
});
test('AI instructions and internal work notes never enter the reader or search', async () => {
  assert.equal(manifest.pages.some(page => /DocAI|translation-report|building|performance/.test(page.id)), false);
  assert.match(await readFile(join(output, 'DocAI.md'), 'utf8'), /coding assistants/i);
  assert.equal(docs.search('CYIMG_EDITOR_TESTS', 'en').length, 0);
});
test('localized annotations, diagrams and gallery use the actual CyDoc compiler', () => {
  assert.match(docs.resolve('layers', 'fr').page.html, /<svg role="img"/);
  assert.match(docs.resolve('overview', 'fr').page.html, /Pellicule/);
  assert.match(docs.resolve('overview', 'en').page.html, /Filmstrip/);
  assert.match(docs.resolve('workspaces', 'en').page.html, /data-cydoc-module="gallery"/);
});
test('only image and gallery are distributed, with the current wide-sidebar reader', async () => {
  await assert.rejects(readFile(new URL('node_modules/three/package.json', import.meta.url)));
  const names = await readdir(join(output, 'DocData'));
  assert.deepEqual(names.filter(x => /^media-.*\.js$/.test(x)).sort(), ['media-gallery.js', 'media-image.js']);
  assert.match(await readFile(join(output, 'Documentation.html'), 'utf8'), /DocData\/viewer.js/);
  assert.match(await readFile(join(output, 'DocData/viewer.css'), 'utf8'), /--sidebar-width:\s*330px/);
});
