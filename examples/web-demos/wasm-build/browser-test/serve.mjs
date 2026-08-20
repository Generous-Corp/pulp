import http from "node:http"; import { readFile } from "node:fs/promises"; import { extname } from "node:path";
import { LOOPBACK_HOST, canonicalRoot, decodeLocalRequestPath, resolveCanonicalAsset, sendFixedText } from "../../tools/local-http-security.mjs";
const ROOT = new URL(".", import.meta.url).pathname;
const ROOT_CANONICAL = canonicalRoot(ROOT);
const MIME = {".html":"text/html",".js":"text/javascript",".mjs":"text/javascript",".wasm":"application/wasm",".json":"application/json"};
const srv = http.createServer(async (req,res)=>{
  const requestPath = decodeLocalRequestPath(req.url);
  if(!requestPath)return sendFixedText(res,400,"bad request");
  const segments=requestPath.segments.length?requestPath.segments:["index.html"];
  const fp=resolveCanonicalAsset(ROOT_CANONICAL,segments);
  if(!fp)return sendFixedText(res,404,"not found");
  try{ const data = await readFile(fp);
    // COOP/COEP set in case threads are ever used; harmless for single-thread.
    res.writeHead(200,{"content-type":MIME[extname(fp)]||"application/octet-stream",
      "cross-origin-opener-policy":"same-origin","cross-origin-embedder-policy":"require-corp"});
    res.end(data);
  }catch{ sendFixedText(res,404,"not found"); }
});
srv.listen(8731, LOOPBACK_HOST, ()=>console.log("WAM harness on http://127.0.0.1:8731/"));
