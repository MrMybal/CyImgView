// CyDoc public viewer API with the exporter's classic-script resource providers.
// Provider adapted from CyDoc web/boot.mjs (MIT, see CyDoc-LICENSE.txt).
import { mountViewer } from 'cydoc/viewer';
const options = globalThis.CYDOC_OPTIONS;
const pending = new Map();
function localScript(file) {
  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = file;
    script.onload = () => { script.remove(); resolve(); };
    script.onerror = () => { script.remove(); reject(new Error(`Cannot load local resource: ${file}`)); };
    document.head.append(script);
  });
}
function loadAsset(src) {
  if (pending.has(src)) return pending.get(src);
  const file = options.assets?.[src];
  if (!file) return Promise.reject(new Error(`Asset is not packaged: ${src}`));
  const result = localScript(file).then(() => {
    const bytes = globalThis.CYDOC_ASSETS?.[src];
    if (globalThis.CYDOC_ASSETS) delete globalThis.CYDOC_ASSETS[src];
    if (!bytes) throw new Error(`Invalid packaged asset: ${src}`);
    return bytes;
  }).finally(() => pending.delete(src));
  pending.set(src, result);
  return result;
}
const modules = Object.fromEntries(options.modules.map(name => [name,
  () => localScript(`media-${name}.js`).then(() => globalThis[`CyDocMedia_${name}`].default)
]));
let savedTheme;
try { savedTheme = localStorage.getItem('cydoc:theme'); } catch { /* The reader reports unavailable storage. */ }
mountViewer(document.querySelector('#app'), globalThis.CYDOC_DATA, {
  homePage: options.homePage,
  initialTheme: savedTheme ? undefined : 'nord',
  storageId: 'cyimgview-manual',
  modules,
  loadAsset
});
