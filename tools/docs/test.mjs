import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { createLink, parseLink } from 'cydoc';
import { verifyManual } from './verify.mjs';
const output = fileURLToPath(new URL('../../build/docs/export/', import.meta.url));
const { docs, manifest, report } = await verifyManual(output);
test('all twelve interface languages retain their original shortcut help', () => {
  assert.equal(manifest.locales.length, 12);
  for (const locale of manifest.locales) {
    const result = docs.resolve('shortcuts', locale.code);
    assert.equal(result.fallback, false);
    assert.match(result.page.html, /F1/);
  }
  assert.match(docs.resolve('shortcuts', 'de').page.html, /Strg\+O/);
});
test('detailed guides fall back honestly and have complete tracked French translations', () => {
  assert.equal(docs.resolve('layers', 'de').page.locale, 'en');
  assert.equal(docs.resolve('layers', 'de').fallback, true);
  assert.equal(docs.resolve('layers', 'fr').fallback, false);
  assert.equal(report.missing.some(item => item.locale === 'fr'), false);
  assert.equal(report.outdated.length + report.untracked.length, 0);
});
test('stable page, section and media links survive route round trips', () => {
  const route = { locale: 'fr', pageId: 'black-white', anchor: 'compare', state: { media: 'color-comparison', wipe: '35' } };
  const link = createLink(route, 'file:///C:/Apps/CyImgView/help/index.html');
  assert.deepEqual(parseLink(link.slice(link.indexOf('#'))), route);
  const image = docs.resolve('black-white', 'fr').page;
  assert.match(image.html, /data-cydoc-id="color-comparison"/);
});
test('legacy page addresses point to their replacements', () => {
  assert.equal(docs.canonicalRoute({ locale: 'fr', pageId: 'mode-edition' }).pageId, 'edit');
  assert.equal(docs.canonicalRoute({ locale: 'en', pageId: 'write-a-plugin' }).pageId, 'decoder-api');
});
test('only reviewed SDK headers and bundled examples are indexed', async () => {
  const info = JSON.parse(await readFile(resolve(output, 'build-info.json'), 'utf8'));
  assert.equal(info.sdk.length, 9);
  const sources = manifest.pages.filter(page => page.locale === 'en' && page.kind === 'source');
  assert.equal(sources.length, info.sdk.length);
  for (const page of sources) assert.match(page.source, /^api\/(include|plugins)\//);
  assert.equal(sources.some(page => /third_party|viewer\.cpp|settings\.ini/i.test(page.source)), false);
});
test('diagrams, rich image annotations and gallery use the real CyDoc compiler', () => {
  assert.match(docs.resolve('layers', 'fr').page.html, /<svg role="img"/);
  assert.match(docs.resolve('overview', 'fr').page.html, /filmstrip/);
  assert.match(docs.resolve('workspaces', 'en').page.html, /data-cydoc-module="gallery"/);
});
test('no Three.js installation or mesh/video/GIF/graph readers are distributed', async () => {
  await assert.rejects(readFile(fileURLToPath(new URL('node_modules/three/package.json', import.meta.url))));
  const names = await readdir(output);
  assert.deepEqual(names.filter(name => /^media-.*\.js$/.test(name)).sort(), ['media-gallery.js', 'media-image.js']);
});
