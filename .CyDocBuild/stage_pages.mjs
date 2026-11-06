// Public hosting adapter only. The application keeps the official export unchanged.
import { cp, mkdir, rm } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, resolve, relative, isAbsolute } from 'node:path';
import { verifyManual } from './verify.mjs';
const workspace = dirname(fileURLToPath(import.meta.url));
const root = resolve(workspace, '..');
const output = resolve(root, 'build/pages');
const delta = relative(resolve(root, 'build'), output);
if (delta !== 'pages' || isAbsolute(delta)) throw new Error('Unsafe Pages staging directory');
await verifyManual(resolve(root, 'Documentation'));
await rm(output, { recursive: true, force: true });
await mkdir(output, { recursive: true });
await cp(resolve(root, 'Documentation'), output, { recursive: true });
await cp(resolve(workspace, 'compat-index.html'), resolve(output, 'index.html'));
console.log(`Pages staging: ${output}; documentation export only, with legacy root URL compatibility.`);
