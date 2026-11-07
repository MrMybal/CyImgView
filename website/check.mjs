import assert from 'node:assert/strict';
import {readFile,readdir,stat} from 'node:fs/promises';
import {resolve,join} from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {createHash} from 'node:crypto';
import {createRequire} from 'node:module';
import {copy} from './content.mjs';
const root=fileURLToPath(new URL('..',import.meta.url));
const require=createRequire(new URL('../.CyDocBuild/package.json',import.meta.url));
const {Window}=await import(pathToFileURL(require.resolve('happy-dom')).href);
const output=resolve(root,'build/pages');
function keys(value,path='') {
  return Object.entries(value).flatMap(([key,item])=> typeof item==='string' ? [path+key] : keys(item,`${path}${key}.`)).sort();
}
assert.deepEqual(keys(copy.en),keys(copy.fr),'Translation fields differ');
const html=await readFile(join(output,'index.html'),'utf8');
const manifest=JSON.parse(await readFile(join(output,'showcase-manifest.json'),'utf8'));
const documentation=JSON.parse(await readFile(join(output,'DocData/manifest.json'),'utf8'));
const window=new Window({settings:{disableJavaScriptFileLoading:true,enableJavaScriptEvaluation:false,enableImageFileLoading:false,enableCSSFileLoading:false}});
window.document.body.innerHTML=html;
for (const locale of ['en','fr']) {
  const page=window.document.querySelector(`#locale-${locale}`).content;
  assert.equal(page.querySelectorAll('h1').length,1);
  assert.equal(page.querySelectorAll('[role=tab]').length,6);
  assert.equal(page.querySelectorAll('[role=tabpanel]').length,6);
  assert.ok(page.querySelector('.header-actions .github-button'));
  const ids=[...page.querySelectorAll('[id]')].map(item=>item.id);
  assert.equal(ids.length,new Set(ids).size,`Duplicate ${locale} anchors`);
  for(const anchor of page.querySelectorAll('a[href]')) {
    const href=anchor.getAttribute('href');
    if(href.startsWith('#') && href.length>1) assert.ok(ids.includes(href.slice(1)),`Missing section ${href}`);
    if(href.startsWith('Documentation.html#')) {
      const [,code,pageId,section]=href.split('#')[1].split('/');
      assert.equal(code,locale,'Wrong documentation language');
      const target=documentation.pages.find(item=>item.id===pageId && item.locale===code);
      assert.ok(target,`Invalid reader route ${href}`);
      if(section) assert.ok(target.headings.some(item=>item.id===section) || target.anchors?.includes(section),`Invalid reader section ${href}`);
    }
    if(href.includes('/releases/download/')) assert.ok(href.includes(`/v${manifest.version}/CyImgView-${manifest.version}-`));
  }
  for(const image of page.querySelectorAll('img[src]')) {
    assert.ok(image.getAttribute('src')?.startsWith('site-assets/'));
    assert.ok(Number(image.getAttribute('width'))>0 && Number(image.getAttribute('height'))>0,'Missing image dimensions');
    if(image.closest('.shot')) assert.ok(image.alt.length>8,'Screenshot needs a useful caption');
    await stat(join(output,image.getAttribute('src')));
  }
  const hero=page.querySelector('.hero img');
  assert.equal(hero.loading,'eager'); assert.equal(hero.getAttribute('fetchpriority'),'high');
}
for(const url of [...Object.values(manifest.resources),...Object.values(manifest.assets).map(item=>item.url)]) {
  const bytes=await readFile(join(output,url));
  assert.ok(url.includes('.'+createHash('sha256').update(bytes).digest('hex').slice(0,12)+'.'),`Unversioned resource ${url}`);
}
async function compareExport(directory,prefix='') {
  for(const entry of await readdir(directory,{withFileTypes:true})) {
    const path=join(prefix,entry.name), original=join(directory,entry.name);
    if(entry.isDirectory()) await compareExport(original,path);
    else assert.equal(createHash('sha256').update(await readFile(join(output,path))).digest('hex'),createHash('sha256').update(await readFile(original)).digest('hex'),`CyDoc export modified: ${path}`);
  }
}
await compareExport(resolve(root,'Documentation'));
assert.ok(!/fetch\s*\(|XMLHttpRequest|https:\/\//.test(await readFile(join(root,'website/app.js'),'utf8')),'Runtime should have no remote dependencies');
const forbidden=['.CyDocBuild','src','plugins','node_modules','.git'];
for(const name of await readdir(output)) assert.ok(!forbidden.includes(name),`Private tree exported: ${name}`);
await window.happyDOM.abort(); window.close();
console.log(`Showcase checks: complete EN/FR, six workspaces, local versioned resources, all links and ${documentation.pages.length} unchanged CyDoc pages valid.`);
