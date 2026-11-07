// Application-owned inputs, official CyDoc exporter. No runtime network or source listings.
import { readFile, writeFile, mkdir, cp, rm, readdir } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, resolve, relative, join, isAbsolute } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
const workspace = dirname(fileURLToPath(import.meta.url));
const root = resolve(workspace, '..');
const args = process.argv.slice(2);
if (Number(process.versions.node.split('.')[0]) < 22) throw new Error('Documentation generation requires Node.js 22 or later.');
function run(command, values, cwd = workspace) {
  const result = spawnSync(command, values, { cwd, stdio: 'inherit', shell: process.platform === 'win32' && command.endsWith('.cmd') });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`Documentation command failed: ${command}`);
}
function inside(path) {
  const full = resolve(path), delta = relative(root, full);
  if (!delta || delta.startsWith('..') || isAbsolute(delta)) throw new Error(`Path leaves the application workspace: ${path}`);
  return full;
}
async function freshGenerated(path) {
  const full = inside(path);
  if (!full.startsWith(join(workspace, 'generated'))) throw new Error('Only private generated inputs can be cleared.');
  await rm(full, { recursive: true, force: true }); await mkdir(full, { recursive: true });
}
async function files(path) {
  const result = [];
  for (const entry of await readdir(path, { withFileTypes: true })) {
    if (entry.isDirectory() && ['node_modules', '.npm-cache', 'generated', 'internal'].includes(entry.name)) continue;
    const name = join(path, entry.name);
    if (entry.isDirectory()) result.push(...await files(name)); else result.push(name);
  }
  return result.sort();
}
const header = await readFile(join(root, 'src/version.h'), 'utf8');
const version = ['MAJOR', 'MINOR', 'PATCH'].map(part => header.match(new RegExp(`CYIMG_VERSION_${part} (\\d+)`))[1]).join('.');
const publicHeaders = ['include/cyimg_plugin.h', 'include/cyimg_edit_plugin.h', 'include/cyimg_metadata_plugin.h'];
const inputs = [...await files(workspace), ...publicHeaders.map(path => join(root, path)), join(root, 'src/version.h'),
  ...await files(join(root, 'src/lang'))];
const hash = createHash('sha256');
for (const path of inputs) { hash.update(relative(root, path)); hash.update(await readFile(path)); }
const fingerprint = hash.digest('hex');
const configPath = join(workspace, 'cydoc.config.json');
const config = JSON.parse(await readFile(configPath, 'utf8'));
const output = args.includes('--out') ? inside(resolve(root, args[args.indexOf('--out') + 1])) : join(root, 'Documentation');
const outputPath = relative(root, output).replaceAll('\\', '/');
if (outputPath !== 'Documentation' && !/^(build|dist)\/.+/.test(outputPath)) throw new Error('Output must be Documentation/ or a subdirectory of build/ or dist/.');
const infoPath = join(output, 'DocData/build-info.json');
if (!args.includes('--force') && !args.includes('--check') && existsSync(infoPath)) {
  try {
    const info = JSON.parse(await readFile(infoPath, 'utf8'));
    if (info.fingerprint === fingerprint && info.version === version) {
      const { verifyManual } = await import('./verify.mjs'); await verifyManual(output);
      console.log(`CyImgView documentation ${version}: up to date`); process.exit(0);
    }
  } catch (error) { console.log(`Rebuilding documentation: ${error.message}`); }
}
if (!existsSync(join(workspace, 'node_modules/cydoc/dist/index.js')))
  run(process.platform === 'win32' ? 'npm.cmd' : 'npm', ['ci', '--no-audit', '--no-fund', '--cache', '.npm-cache']);
const generated = join(workspace, 'generated');
await freshGenerated(generated);
const content = join(generated, 'manual');
await cp(join(workspace, 'manual'), content, { recursive: true });
function langTable(locale) {
  return Promise.all(inputs.filter(path => path.includes(join('src', 'lang')) && path.endsWith(`.${locale}.lang`)).map(async path => {
    const values = {};
    for (const line of (await readFile(path, 'utf8')).split(/\r?\n/)) {
      const field = line.match(/^([^#@][^=]*?)\s*=\s*(.*)$/); if (field) values[field[1].trim()] = field[2].trim();
    }
    return values;
  })).then(tables => Object.assign({}, ...tables));
}
const shortcuts = [
  ['viewer.help.k.nav', '', 'viewer.help.d.nav'], ['', 'N', 'viewer.help.d.filter'], ['', 'D', 'viewer.help.d.slideshow'],
  ['viewer.help.k.copy', '', 'viewer.help.d.copy'], ['viewer.help.k.channels', '', 'viewer.help.d.channels'],
  ['viewer.help.k.anim', '', 'viewer.help.d.anim'], ['viewer.help.k.home_end', '', 'viewer.help.d.home_end'],
  ['viewer.help.k.zoom', '', 'viewer.help.d.zoom'], ['viewer.help.k.drag', '', 'viewer.help.d.drag'],
  ['viewer.help.k.dblclick', '', 'viewer.help.d.dblclick'], ['', '0 / 1', 'viewer.help.d.fit'],
  ['viewer.help.k.rotate', '', 'viewer.help.d.rotate'], ['viewer.help.k.fullscreen', '', 'viewer.help.d.fullscreen'],
  ['', 'T', 'viewer.help.d.strip'], ['', 'I', 'viewer.help.d.info'], ['', 'Ctrl+O', 'viewer.help.d.open'],
  ['', 'Ctrl+S', 'viewer.help.d.save_as'], ['openwith.help.key', '', 'openwith.help.desc'], ['', 'Ctrl+U', 'viewer.help.d.update'],
  ['', 'P', 'viewer.help.d.settings'], ['', 'E', 'viewer.help.d.edit'], ['', 'F5', 'viewer.help.d.reload'],
  ['viewer.help.k.mouse_side', '', 'viewer.help.d.mouse_side'], ['viewer.help.k.esc', '', 'viewer.help.d.esc']
];
for (const { code } of config.locales) {
  const t = await langTable(code), fr = code === 'fr';
  let body = `---\nid: shortcuts\ntitle: ${t['viewer.help.title']}\ndescription: ${fr ? 'Retrouvez les raccourcis pour parcourir et examiner les images.' : 'Find shortcuts to browse and inspect images.'}\nsection: ${fr ? 'Références' : 'Reference'}\nparent: viewer\norder: 110\n${fr ? 'sourceRevision' : 'revision'}: 2\n---\n\n## ${t['viewer.help.title']} {#viewer}\n\n| ${fr ? 'Touche | Action' : 'Key | Action'} |\n|---|---|\n`;
  for (const [key, fixed, description] of shortcuts) {
    let label = key ? t[key] : fixed;
    if (!label || !t[description]) throw new Error(`Missing native shortcut translation: ${code}/${description}`);
    if (label.startsWith('Ctrl')) label = t['viewer.key.ctrl'] + label.slice(4);
    body += `| ${label.replaceAll('|', '\\|')} | ${t[description].replaceAll('|', '\\|')} |\n`;
  }
  body += `| F1 | ${t['viewer.btn.help']} |\n\n`;
  body += fr ? '[Commandes de l’éditeur](doc:keyboard-reference#editing).\n' : '[Editor keyboard commands](doc:keyboard-reference#editing).\n';
  await writeFile(join(content, code, 'shortcuts.md'), body);
}
const { generateApi } = await import('./generate_api.mjs');
await generateApi({ root, workspace, content });
const generatedConfig = { ...config, version, out: relative(workspace, output).replaceAll('\\', '/') };
// This is a generated configuration input, never a modification of exported HTML or data.
const activeConfig = join(workspace, 'cydoc.active.json');
await writeFile(activeConfig, JSON.stringify(generatedConfig, null, 2));
try { run(process.execPath, [join(workspace, 'node_modules/cydoc/tools/cli.mjs'), args.includes('--check') ? 'check' : 'build', activeConfig]); }
finally { await rm(activeConfig, { force: true }); }
await cp(join(workspace, 'node_modules/cydoc/LICENSE'), join(output, 'DocData/CyDoc-LICENSE.txt'));
await cp(join(root, 'LICENSE'), join(output, 'DocData/CyImgView-LICENSE.txt'));
await writeFile(infoPath, JSON.stringify({ version, modules: config.modules, publicApiOnly: true,
  cydoc: '0.1.0', commit: '6ddf9a582defb2f11fbb3f992afef6f98dcadc93', fingerprint }, null, 2));
const { verifyManual } = await import('./verify.mjs'); await verifyManual(output);
console.log(`CyImgView documentation ${version}: ${join(output, 'Documentation.html')}`);
