// Generate a complete, serverless CyDoc export. Node/npm are build tools only.
import { readFile, writeFile, mkdir, cp, rm, readdir, stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, resolve, relative, join, isAbsolute } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const tooling = join(root, 'tools/docs');
const work = join(root, 'build/docs');
const args = process.argv.slice(2);
if (Number(process.versions.node.split('.')[0]) < 22) throw new Error('The manual requires Node.js 22 or later to build.');
function run(command, values, cwd = root) {
  const result = spawnSync(command, values, { cwd, stdio: 'inherit', shell: process.platform === 'win32' && command.endsWith('.cmd') });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`Documentation command failed: ${command}`);
}
function checked(path) {
  const full = resolve(path), delta = relative(root, full);
  if (!delta || delta.startsWith('..') || isAbsolute(delta)) throw new Error(`Documentation path leaves the workspace: ${path}`);
  return full;
}
async function fresh(path) { checked(path); await rm(path, { recursive: true, force: true }); await mkdir(path, { recursive: true }); }
async function files(path) {
  const result = [];
  for (const entry of await readdir(path, { withFileTypes: true })) {
    const name = join(path, entry.name);
    if (entry.isDirectory()) result.push(...await files(name)); else result.push(name);
  }
  return result.sort();
}
const header = await readFile(join(root, 'src/version.h'), 'utf8');
const version = ['MAJOR', 'MINOR', 'PATCH'].map(key => header.match(new RegExp(`CYIMG_VERSION_${key} (\\d+)`))[1]).join('.');
const config = JSON.parse(await readFile(join(root, 'cydoc.config.json'), 'utf8'));
const sdk = ['include/cyimg_plugin.h', 'include/cyimg_edit_plugin.h', 'include/cyimg_plugin_i18n.h', 'plugins/fx_extra/fx_extra.cpp',
  'plugins/annotate/annotate.cpp', 'plugins/share_actions/share_actions.cpp', 'plugins/bw_mode/bw_mode.cpp', 'plugins/hlsl/hlsl.cpp', 'plugins/hlsl/warmth.hlsl'];
const inputs = [...await files(join(root, 'docs/manual')), ...await files(join(root, 'docs/assets')),
  ...await files(join(root, 'docs/screenshots')), ...await files(tooling).then(list => list.filter(name => !name.includes(`${join('docs', 'node_modules')}`))),
  ...sdk.map(name => join(root, name)), ...await files(join(root, 'src/lang')), join(root, 'src/version.h'),
  join(root, 'cydoc.config.json'), fileURLToPath(import.meta.url)];
const hash = createHash('sha256');
for (const name of inputs) { hash.update(relative(root, name)); hash.update(await readFile(name)); }
const fingerprint = hash.digest('hex');
const output = args.includes('--out') ? checked(args[args.indexOf('--out') + 1]) : join(root, 'build/Release/bin/help');
const outputPath = relative(root, output).replaceAll('\\', '/');
if (!/^(build|dist)\/.+/.test(outputPath) || outputPath === 'build/docs' || outputPath.startsWith('build/docs/'))
  throw new Error('Use an output subdirectory of build/ or dist/, outside the internal build/docs workspace.');
const stamp = join(work, 'fingerprint');
const current = existsSync(stamp) ? (await readFile(stamp, 'utf8')).trim() : '';
if (!args.includes('--check') && !args.includes('--force') && current === fingerprint && existsSync(join(output, 'index.html'))) {
  try {
    const { verifyManual } = await import('./docs/verify.mjs');
    await verifyManual(output);
    console.log(`CyImgView manual ${version}: up to date`);
    process.exit(0);
  } catch (error) { console.log(`Repairing incomplete manual: ${error.message}`); }
}
if (!existsSync(join(tooling, 'node_modules/cydoc/dist/index.js'))) run(process.platform === 'win32' ? 'npm.cmd' : 'npm', ['ci', '--no-audit', '--no-fund'], tooling);
await fresh(work);
for (const name of ['content', 'api', 'assets']) await mkdir(join(work, name), { recursive: true });
await cp(join(root, 'docs/manual'), join(work, 'content'), { recursive: true });
await cp(join(root, 'docs/assets'), join(work, 'assets'), { recursive: true });
await cp(join(root, 'docs/screenshots'), join(work, 'assets/screenshots'), { recursive: true });
// Copy a reviewed source allowlist only. CyDoc indexes and ships source file contents.
for (const name of sdk) { const dest = join(work, 'api', name); await mkdir(dirname(dest), { recursive: true }); await cp(join(root, name), dest); }
function langTable(code) {
  const table = {};
  return Promise.all(inputs.filter(name => name.includes(`${join('src', 'lang')}`) && name.endsWith(`.${code}.lang`)).map(async name => {
    for (const line of (await readFile(name, 'utf8')).split(/\r?\n/)) {
      const match = line.match(/^([^#@][^=]*?)\s*=\s*(.*)$/);
      if (match) table[match[1].trim()] = match[2].trim();
    }
  })).then(() => table);
}
const fallback = await langTable('en');
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
for (const locale of config.locales) {
  const t = { ...fallback, ...await langTable(locale.code) }, fr = locale.code === 'fr';
  let content = `---\nid: shortcuts\ntitle: ${t['viewer.help.title']}\nsection: ${fr ? 'Références' : 'Reference'}\norder: 110\n`;
  content += locale.code === 'en' ? 'revision: 1\n' : 'sourceRevision: 1\n';
  content += `---\n\n## ${t['viewer.help.title']} {#viewer}\n\n| ${fr ? 'Touche | Action' : 'Key | Action'} |\n|---|---|\n`;
  const escape = value => value.replaceAll('|', '\\|');
  for (const [key, fixed, description] of shortcuts) {
    let label = key ? t[key] : fixed;
    if (label.startsWith('Ctrl')) label = t['viewer.key.ctrl'] + label.slice(4);
    content += `| ${escape(label)} | ${escape(t[description])} |\n`;
  }
  content += `| F1 | ${t['viewer.btn.help']} |\n\n`;
  content += fr ? '[Commandes détaillées de la visionneuse et de l’éditeur](doc:keyboard-reference#editing).\n' : '[Detailed viewer and editor commands](doc:keyboard-reference#editing).\n';
  await mkdir(join(work, 'content', locale.code), { recursive: true });
  await writeFile(join(work, 'content', locale.code, 'shortcuts.md'), content);
}
config.version = version;
// Relative paths retain stable source-page IDs. The exporter uses only its supported settings.
const configFile = join(work, 'cydoc.config.json');
const generatedConfig = { ...config, content: 'content', assets: 'assets', sources: ['api'],
  sourceIndex: { enabled: true, includeDirs: ['api/include'] }, out: 'export' };
await writeFile(configFile, JSON.stringify(generatedConfig, null, 2));
run(process.execPath, [join(tooling, 'node_modules/cydoc/tools/cli.mjs'), args.includes('--check') ? 'check' : 'build', configFile]);
const exported = join(work, 'export');
// Application-owned shell and public API adapter; CyDoc data and module providers are unmodified.
const { build } = await import('./docs/node_modules/esbuild/lib/main.js');
await build({ entryPoints: [join(tooling, 'reader.mjs')], bundle: true, format: 'iife', minify: true,
  target: 'es2020', outfile: join(exported, 'app-reader.js'), legalComments: 'eof' });
await cp(join(tooling, 'reader.html'), join(exported, 'index.html'));
await cp(join(tooling, 'node_modules/cydoc/LICENSE'), join(exported, 'CyDoc-LICENSE.txt'));
await cp(join(root, 'LICENSE'), join(exported, 'CyImgView-LICENSE.txt'));
await writeFile(join(exported, 'build-info.json'), JSON.stringify({ version, modules: config.modules, sdk,
  cydoc: '0.1.0', commit: '774f2a7eabf7dd181a98ffc5add115b580b67f42', fingerprint }, null, 2));
const { verifyManual } = await import('./docs/verify.mjs');
await verifyManual(exported);
await fresh(output);
await cp(exported, output, { recursive: true });
await writeFile(stamp, fingerprint);
console.log(`CyImgView manual ${version}: ${output}`);
