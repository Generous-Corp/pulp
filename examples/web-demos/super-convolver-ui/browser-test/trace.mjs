import { chromium } from "playwright-core";
import { createServer } from "node:http";
import { existsSync, readFileSync } from "node:fs";
import { extname, join, resolve } from "node:path";
const BUILD = resolve(process.argv[2]);
const MIME={".js":"text/javascript",".mjs":"text/javascript",".wasm":"application/wasm",".data":"application/octet-stream",".html":"text/html"};
const html = `<!doctype html><canvas id=pulp-ui-canvas width=640 height=400></canvas><script type=module>
import Module from "./PulpSuperConvolverUi.js";
Error.stackTraceLimit=4000;window.__err=null;
window.addEventListener("error",e=>{window.__err=(e.error&&e.error.stack)||e.message;});
try{ const M=await Module({canvas:document.getElementById("pulp-ui-canvas")});
  window.__M=M; if(M._pulp_ui_init){ const sel=M.stringToNewUTF8("#pulp-ui-canvas");
    M._pulp_ui_init(sel,640,400,2); M._free(sel);} window.__ok=true;
}catch(e){ window.__err=(e&&e.stack)||String(e);}
</script>`;
const srv=createServer((q,s)=>{let u=(q.url||"/").split("?")[0]; if(u==="/"){s.writeHead(200,{"Content-Type":"text/html","Cross-Origin-Opener-Policy":"same-origin","Cross-Origin-Embedder-Policy":"require-corp"});return s.end(html);} let p=join(BUILD,u); if(!existsSync(p)){s.writeHead(404);return s.end();} s.writeHead(200,{"Content-Type":MIME[extname(p)]||"application/octet-stream","Cross-Origin-Opener-Policy":"same-origin","Cross-Origin-Embedder-Policy":"require-corp","Cross-Origin-Resource-Policy":"cross-origin"});s.end(readFileSync(p));});
await new Promise(r=>srv.listen(0,r));
const b=await chromium.launch({executablePath:"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",headless:true,args:["--enable-unsafe-swiftshader"]});
const pg=await b.newPage();
const trace=[]; pg.on("console",m=>trace.push("[console] "+m.text().slice(0,200)));
await pg.goto("http://localhost:"+srv.address().port+"/",{waitUntil:"load"});
await pg.evaluate(()=>new Promise(r=>setTimeout(r,4000)));
const err=await pg.evaluate(()=>window.__err);
console.log("MOUNTED OK:",await pg.evaluate(()=>window.__ok===true));
console.log("=== ERROR STACK ===\n"+(err||"(none)"));
console.log("=== last console lines ===\n"+trace.slice(-12).join("\n"));
await b.close();srv.close();
