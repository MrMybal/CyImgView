import { readFile, readdir, stat } from 'node:fs/promises';
import { join } from 'node:path';
import { createDocumentation, hasAnchor, parseLink } from 'cydoc';
export const helpRoutes = {
  overview: ['start'], quickstart: ['open'], viewer: ['navigation', 'channels', 'animation', 'information'],
  'new-document': ['templates'], edit: ['tools', 'objects', 'paint', 'navigation'],
  layers: ['layers', 'masks', 'blending'], effects: ['gallery', 'apply', 'extensions'],
  develop: ['adjust'], 'black-white': ['cancel'], export: ['project', 'export'],
  shortcuts: ['viewer'], settings: ['settings'], plugins: ['overview', 'api', 'modes'],
  troubleshooting: ['help'], hlsl: ['contract']
};
export async function verifyManual(directory) {
  const manifest = JSON.parse(await readFile(join(directory, 'manifest.json'), 'utf8'));
  const docs = createDocumentation(manifest);
  const report = JSON.parse(await readFile(join(directory, 'translation-report.json'), 'utf8'));
  if (report.outdated.length || report.untracked.length) throw new Error('Manual has outdated or untracked translations.');
  if (report.missing.some(item => item.locale === 'fr')) throw new Error('A French manual translation is missing.');
  for (const locale of manifest.locales) {
    if (!manifest.pages.some(page => page.id === 'shortcuts' && page.locale === locale.code)) throw new Error(`Lost shortcut locale: ${locale.code}`);
    for (const [page, anchors] of Object.entries(helpRoutes)) for (const anchor of anchors) {
      const target = docs.resolve(page, locale.code);
      if (!target || !hasAnchor(target.page, anchor)) throw new Error(`Invalid native help route: ${locale.code}/${page}/${anchor}`);
    }
  }
  // Both complete translations expose the same public section and media addresses.
  for (const original of manifest.pages.filter(page => page.locale === 'en' && page.kind !== 'source' && page.id !== 'translation-report')) {
    const translated = manifest.pages.find(page => page.id === original.id && page.locale === 'fr');
    if (!translated) throw new Error(`Missing French page: ${original.id}`);
    const ids = page => [...page.headings.map(h => h.id), ...(page.anchors || []),
      ...[...page.html.matchAll(/data-cydoc-id="([^"]+)"/g)].map(match => match[1])].sort();
    if (JSON.stringify(ids(original)) !== JSON.stringify(ids(translated))) throw new Error(`Section IDs differ between EN/FR: ${original.id}`);
  }
  const exports = await readdir(directory);
  const media = exports.filter(name => /^media-.*\.js$/.test(name)).sort();
  if (JSON.stringify(media) !== JSON.stringify(['media-gallery.js', 'media-image.js'])) throw new Error(`Unexpected reader modules: ${media}`);
  for (const name of ['index.html', 'viewer.css', 'data.js', 'app-reader.js', 'CyDoc-LICENSE.txt', 'CyImgView-LICENSE.txt'])
    if (!(await stat(join(directory, name))).isFile()) throw new Error(`Missing export file: ${name}`);
  const html = await readFile(join(directory, 'index.html'), 'utf8');
  if (/(?:src|href)=["'](?:https?:|\/\/)/i.test(html) || /type=["']module["']/.test(html)) throw new Error('The local reader has a remote or ESM-only bootstrap.');
  for (const match of html.matchAll(/(?:src|href)="([^"#]+)"/g))
    if (!(await stat(join(directory, match[1]))).isFile()) throw new Error(`Broken local shell resource: ${match[1]}`);
  const bundle = await readFile(join(directory, 'app-reader.js'), 'utf8');
  if (/\bfetch\s*\(|https?:\/\/(?!www\.w3\.org)/.test(bundle)) throw new Error('The reader bundle contains an unexpected network dependency.');
  for (const page of manifest.pages) {
    for (const match of page.html.matchAll(/data-config="([^"]+)"/g)) {
      const config = JSON.parse(match[1].replaceAll('&quot;', '"').replaceAll('&amp;', '&').replaceAll('&#39;', "'").replaceAll('&lt;', '<').replaceAll('&gt;', '>'));
      for (const item of [config, ...(config.items || [])]) for (const key of ['src', 'compare']) if (item[key]) {
        if (!item[key].startsWith('assets/') || item[key].includes('..')) throw new Error(`Nonlocal media: ${item[key]}`);
        if (!(await stat(join(directory, item[key]))).isFile()) throw new Error(`Missing media: ${item[key]}`);
      }
    }
  }
  console.log(`Manual checks: ${manifest.pages.length} pages; ${manifest.locales.length} locales; image + gallery; native routes and local resources valid.`);
  return { docs, manifest, report };
}
