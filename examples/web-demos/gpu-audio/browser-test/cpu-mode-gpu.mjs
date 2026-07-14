import { chromium } from "playwright-core";
import { createServer } from "node:http";
import { existsSync, readFileSync } from "node:fs";
import { extname, join, resolve } from "node:path";
const SITE = resolve("../../wclap-build/cloudflare/public");
const MIME = { ".html":"text/html", ".js":"text/javascript", ".mjs":"text/javascript", ".wasm":"application/wasm", ".data":"application/octet-stream", ".png":"image/png", ".css":"text/css" };
const server = createServer((q,s)=>{const u=(q.url||"/").split("?")[0];let p=join(SITE,decodeURIComponent(u));if(p.endsWith("/"))p=join(p,"index.html");if(!existsSync(p)){s.writeHead(404);s.end();return;}s.writeHead(200,{"Content-Type":MIME[extname(p)]||"application/octet-stream","Cross-Origin-Opener-Policy":"same-origin","Cross-Origin-Embedder-Policy":"require-corp","Cross-Origin-Resource-Policy":"cross-origin"});s.end(readFileSync(p));});
await new Promise(r=>server.listen(0,r));
const b = await chromium.launch({ executablePath:"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", headless:true, args:["--autoplay-policy=no-user-gesture-required"] });
const pg = await b.newPage();
await pg.goto("http://localhost:"+server.address().port+"/super-convolver-gpu/",{waitUntil:"load"});
await pg.waitForSelector("#ov-start",{timeout:25000});
await pg.click("#ov-start");
await pg.waitForSelector("#engine",{timeout:60000});          // IR reached the worker
const engine = await pg.evaluate(() => document.querySelector("#engine").value);
const read = () => pg.evaluate(() => {
  const st = window.__gpuStats;   // the worker's OWN counters, out of the SAB
  return st ? { produced: st.produced || 0, submits: st.queueSubmits || 0, expired: st.expired || 0 } : null;
});
console.log("engine select =", engine);
const a = await read();
await pg.evaluate(() => new Promise(r => setTimeout(r, 3000)));
const c = await read();
console.log("t=0  ", JSON.stringify(a));
console.log("t=3s ", JSON.stringify(c));
await b.close(); server.close();
