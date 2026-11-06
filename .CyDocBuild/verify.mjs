import { readFile, readdir, stat } from 'node:fs/promises';
import { join } from 'node:path';
import { createDocumentation, hasAnchor } from 'cydoc';
export const helpRoutes = {
  overview: ['start'], quickstart: ['open'], viewer: ['navigation', 'channels', 'animation', 'information'],
  'new-document': ['templates'], edit: ['tools', 'objects', 'paint', 'navigation', 'gradient'],
  layers: ['layers', 'masks', 'blending'], effects: ['gallery', 'apply', 'extensions'],
  develop: ['adjust'], 'black-white': ['cancel'], export: ['project', 'export'],
  shortcuts: ['viewer'], settings: ['settings'], plugins: ['overview', 'api', 'modes'],
  troubleshooting: ['help'], hlsl: ['contract']
};
export async function verifyManual(directory) {
  const data = join(directory, 'DocData');
  const manifest = JSON.parse(await readFile(join(data, 'manifest.json'), 'utf8'));
  const docs = createDocumentation(manifest);
  const report = JSON.parse(await readFile(join(data, 'translation-report.json'), 'utf8'));
  if (manifest.defaultLocale !== 'en' || JSON.stringify(manifest.locales.map(x => x.code)) !== '["en","fr"]') throw new Error('Only complete English and French manuals may be offered.');
  if (report.missing.length || report.outdated.length || report.untracked.length) throw new Error('Manual translations are incomplete or untracked.');
  for (const locale of manifest.locales) for (const [page, anchors] of Object.entries(helpRoutes)) for (const anchor of anchors) {
    const target = docs.resolve(page, locale.code);
    if (!target || target.fallback || !hasAnchor(target.page, anchor)) throw new Error(`Invalid native help route: ${locale.code}/${page}/${anchor}`);
  }
  const ids = page => [...page.headings.map(h => h.id), ...(page.anchors || []), ...[...page.html.matchAll(/data-cydoc-id="([^"]+)"/g)].map(m => m[1])].sort();
  for (const original of manifest.pages.filter(page => page.locale === 'en')) {
    const translated = manifest.pages.find(page => page.id === original.id && page.locale === 'fr');
    if (!translated || original.parent !== translated.parent || JSON.stringify(ids(original)) !== JSON.stringify(ids(translated))) throw new Error(`Translation structure differs: ${original.id}`);
  }
  for (const page of manifest.pages) {
    if (!page.description?.trim()) throw new Error(`Missing useful tooltip description: ${page.id}/${page.locale}`);
    if (page.kind === 'source' || page.id.startsWith('source-') || ['building', 'performance', 'translation-report', 'DocAI'].includes(page.id)) throw new Error(`Internal content in manual: ${page.id}`);
    if (/CYIMG_EDITOR_TESTS|check_lang\.py|build_docs\.mjs|npm --prefix|gitleaks|migration inventory/i.test(page.html)) throw new Error(`Work notes in manual: ${page.id}`);
    for (const match of page.html.matchAll(/data-config="([^"]+)"/g)) {
      const config = JSON.parse(match[1].replaceAll('&quot;', '"').replaceAll('&amp;', '&').replaceAll('&#39;', "'").replaceAll('&lt;', '<').replaceAll('&gt;', '>'));
      for (const item of [config, ...(config.items || [])]) for (const key of ['src', 'compare']) if (item[key]) {
        if (!item[key].startsWith('assets/') || item[key].includes('..')) throw new Error(`Nonlocal media: ${item[key]}`);
        if (!(await stat(join(data, item[key]))).isFile()) throw new Error(`Missing media: ${item[key]}`);
        if (page.locale === 'fr' && /-en\.png$/.test(item[key])) throw new Error(`English screenshot in French manual: ${page.id}`);
      }
    }
  }
  const media = (await readdir(data)).filter(name => /^media-.*\.js$/.test(name)).sort();
  if (JSON.stringify(media) !== '["media-gallery.js","media-image.js"]') throw new Error(`Unexpected modules: ${media}`);
  for (const name of ['Documentation.html', 'DocAI.md', 'DocData/viewer.css', 'DocData/data.js', 'DocData/viewer.js', 'DocData/CyDoc-LICENSE.txt', 'DocData/CyImgView-LICENSE.txt', 'DocData/assets/app-logo.png'])
    if (!(await stat(join(directory, name))).isFile()) throw new Error(`Missing export file: ${name}`);
  const html = await readFile(join(directory, 'Documentation.html'), 'utf8');
  if (/(?:src|href)=["'](?:https?:|\/\/)/i.test(html) || /type=["']module["']|<iframe|http-equiv=["']refresh/i.test(html)) throw new Error('The local reader must open directly without a remote or ESM bootstrap.');
  for (const match of html.matchAll(/(?:src|href)="([^"#]+)"/g))
    if (!(await stat(join(directory, match[1]))).isFile()) throw new Error(`Broken shell resource: ${match[1]}`);
  for (const name of ['viewer.js', ...media]) if (/\bfetch\s*\(|XMLHttpRequest|new WebSocket/.test(await readFile(join(data, name), 'utf8'))) throw new Error(`Unexpected network loader: ${name}`);
  async function walk(path) {
    for (const entry of await readdir(path, { withFileTypes: true })) {
      const target = join(path, entry.name);
      if (entry.isDirectory()) { if (/node_modules|\.CyDocBuild|SourceFile/.test(entry.name)) throw new Error(`Private tree exported: ${target}`); await walk(target); }
      else if (/\.(?:cpp|hpp|h|hlsl|ps1|py|tgz|ts)$/i.test(entry.name)) throw new Error(`Source file exported: ${target}`);
    }
  }
  await walk(directory);
  console.log(`Manual checks: ${manifest.pages.length} pages; complete EN/FR; image + gallery; public API only; contextual routes and resources valid.`);
  return { docs, manifest, report };
}
