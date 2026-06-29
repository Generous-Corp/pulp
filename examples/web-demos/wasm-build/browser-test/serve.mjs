import http from "node:http"; import { readFile } from "node:fs/promises"; import { extname, join, normalize } from "node:path";
const ROOT = new URL(".", import.meta.url).pathname;
const MIME = {".html":"text/html",".js":"text/javascript",".mjs":"text/javascript",".wasm":"application/wasm",".json":"application/json"};
const srv = http.createServer(async (req,res)=>{
  let p = decodeURIComponent(req.url.split("?")[0]); if(p==="/")p="/index.html";
  const fp = normalize(join(ROOT, p));
  if(!fp.startsWith(ROOT)){res.writeHead(403);return res.end();}
  try{ const data = await readFile(fp);
    // COOP/COEP set in case threads are ever used; harmless for single-thread.
    res.writeHead(200,{"content-type":MIME[extname(fp)]||"application/octet-stream",
      "cross-origin-opener-policy":"same-origin","cross-origin-embedder-policy":"require-corp"});
    res.end(data);
  }catch{ res.writeHead(404); res.end("not found: "+p); }
});
srv.listen(8731, ()=>console.log("WAM harness on http://localhost:8731/"));
