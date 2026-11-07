// Optional maintainer tool. The generated ICO is committed; application builds need no image tools.
// Usage: node tools/build_document_icon.mjs [absolute path to a sharp installation]
import { createRequire } from 'node:module';
import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const require = createRequire(import.meta.url);
const sharp = require(process.argv[2] || 'sharp');
const root = fileURLToPath(new URL('../', import.meta.url));
const source = await readFile(path.join(root, 'src/cyimg-document.svg'));
const sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256];
const images = [];
for (const size of sizes) images.push(await sharp(source, { density: 384 }).resize(size, size).png().toBuffer());
const header = Buffer.alloc(6 + sizes.length * 16);
header.writeUInt16LE(1, 2);
header.writeUInt16LE(sizes.length, 4);
let offset = header.length;
sizes.forEach((size, i) => {
  const p = 6 + i * 16;
  header[p] = header[p + 1] = size === 256 ? 0 : size;
  header.writeUInt16LE(1, p + 4);
  header.writeUInt16LE(32, p + 6);
  header.writeUInt32LE(images[i].length, p + 8);
  header.writeUInt32LE(offset, p + 12);
  offset += images[i].length;
});
await writeFile(path.join(root, 'src/cyimg-document.ico'), Buffer.concat([header, ...images]));
await sharp(source, { density: 192 }).resize(512, 512).png().toFile(path.join(root, 'docs/cyimg-document.png'));
console.log(`CyImg document icon: ${sizes.join(', ')} px, transparent PNG preview in docs/cyimg-document.png`);
