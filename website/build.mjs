import {readFile,writeFile,mkdir,cp} from 'node:fs/promises';
import {resolve,relative,isAbsolute,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {copy} from './content.mjs';
import {renderPage} from './render.mjs';
const source = fileURLToPath(new URL('.',import.meta.url));
const root = resolve(source,'..');
const hash = bytes => createHash('sha256').update(bytes).digest('hex').slice(0,12);
const escapeAttr = value => value.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
export async function buildShowcase(output) {
  const delta = relative(resolve(root,'build'),resolve(output));
  if (!delta || delta.startsWith('..') || isAbsolute(delta)) throw new Error('The showcase output must stay under build/.');
  const assets = {};
  const destination = join(output,'site-assets'); await mkdir(destination,{recursive:true});
  const provenance = JSON.parse(await readFile(join(source,'asset-provenance.json'),'utf8'));
  for (const item of provenance) {
    const bytes = await readFile(join(source,'assets',item.asset));
    const dot = item.asset.lastIndexOf('.');
    const filename = `${item.asset.slice(0,dot)}.${hash(bytes)}${item.asset.slice(dot)}`;
    await cp(join(source,'assets',item.asset),join(destination,filename));
    assets[item.asset] = {url:`site-assets/${filename}`,width:item.width,height:item.height};
  }
  const resources = {};
  for (const [name,ext] of [['style','css'],['app','js']]) {
    const bytes = await readFile(join(source,`${name}.${ext}`));
    resources[name] = `site-assets/${name}.${hash(bytes)}.${ext}`;
    await writeFile(join(output,resources[name]),bytes);
  }
  const header = await readFile(join(root,'src/version.h'),'utf8');
  const version = ['MAJOR','MINOR','PATCH'].map(part=>header.match(new RegExp(`CYIMG_VERSION_${part} (\\d+)`))[1]).join('.');
  const en = renderPage('en',assets,version), fr = renderPage('fr',assets,version);
  const html = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark"><meta name="theme-color" content="#101314"><meta name="description" content="${escapeAttr(copy.en.description)}"><meta property="og:type" content="website"><meta property="og:title" content="${escapeAttr(copy.en.title)}"><meta property="og:description" content="${escapeAttr(copy.en.description)}"><title>${copy.en.title}</title><link rel="icon" href="${assets['logo.png'].url}" type="image/png"><link rel="stylesheet" href="${resources.style}"><script>if(/^#\\/[a-z]{2}\\//.test(location.hash))location.replace('Documentation.html'+location.hash);</script><script src="${resources.app}" defer></script></head><body><div id="page">${en}</div><template id="locale-en" data-title="${escapeAttr(copy.en.title)}" data-description="${escapeAttr(copy.en.description)}">${en}</template><template id="locale-fr" data-title="${escapeAttr(copy.fr.title)}" data-description="${escapeAttr(copy.fr.description)}">${fr}</template><noscript><p>Enable JavaScript to switch languages and enlarge screenshots. The English page and documentation links remain available.</p></noscript></body></html>`;
  await writeFile(join(output,'index.html'),html);
  await writeFile(join(output,'.nojekyll'),'');
  await writeFile(join(output,'showcase-manifest.json'),JSON.stringify({version,locales:['en','fr'],resources,assets},null,2));
  console.log(`Showcase: EN/FR, ${provenance.length} local assets, fingerprinted CSS/JS → ${output}`);
}
