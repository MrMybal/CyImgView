// Preview only the generated public directory, bound to loopback. No application data is served.
import {createServer} from 'node:http';
import {readFile,stat} from 'node:fs/promises';
import {resolve,relative,isAbsolute,extname} from 'node:path';
import {fileURLToPath} from 'node:url';
const directory = fileURLToPath(new URL('../build/pages/',import.meta.url));
const index = process.argv.indexOf('--port');
const port = index < 0 ? 4173 : Number(process.argv[index+1]);
if (!Number.isInteger(port) || port < 1024 || port > 65535) throw new Error('Invalid local preview port');
const types = {'.html':'text/html; charset=utf-8','.css':'text/css; charset=utf-8','.js':'text/javascript; charset=utf-8','.json':'application/json; charset=utf-8','.webp':'image/webp','.png':'image/png','.svg':'image/svg+xml','.md':'text/plain; charset=utf-8','.txt':'text/plain; charset=utf-8'};
const server = createServer(async (request,response) => {
  try {
    const path = decodeURIComponent(new URL(request.url,'http://127.0.0.1').pathname);
    let file = resolve(directory,'.'+path);
    const delta = relative(directory,file);
    if (delta.startsWith('..') || isAbsolute(delta)) { response.writeHead(403);response.end();return; }
    if ((await stat(file)).isDirectory()) file = resolve(file,'index.html');
    const bytes = await readFile(file);
    response.writeHead(200,{'Content-Type':types[extname(file)]||'application/octet-stream','Cache-Control':/\.[a-f0-9]{12}\./.test(file)?'public, max-age=31536000, immutable':'no-cache','X-Content-Type-Options':'nosniff'});
    response.end(request.method==='HEAD'?undefined:bytes);
  } catch { response.writeHead(404,{'Content-Type':'text/plain; charset=utf-8'});response.end('Not found'); }
});
server.listen(port,'127.0.0.1',()=>console.log(`Local: http://127.0.0.1:${port}/`));
process.on('SIGINT',()=>server.close(()=>process.exit(0)));
