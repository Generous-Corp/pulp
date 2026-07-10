// Capture a screenshot of the Pulp Live editor mid-scrub (headless Chrome, CDP).
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { existsSync, writeFileSync } from "node:fs";

const out = process.argv[2] || "/tmp/pulp-live.png";
const PORT = 8796;
const PAGE = `http://127.0.0.1:${PORT}/examples/web-demos/live-kernel-spike/editor.html`;
const EXE = ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/Applications/Chromium.app/Contents/MacOS/Chromium"].find(existsSync);

class CDP { constructor(u){ this.ws=new WebSocket(u); this.id=0; this.p=new Map(); this.open=new Promise((r,j)=>{this.ws.addEventListener("open",r);this.ws.addEventListener("error",e=>j(e));}); this.ws.addEventListener("message",e=>{const m=JSON.parse(e.data); if(m.id&&this.p.has(m.id)){const{res,rej}=this.p.get(m.id);this.p.delete(m.id);m.error?rej(new Error(JSON.stringify(m.error))):res(m.result);}});}
  send(method,params={},sessionId){const id=++this.id;return new Promise((res,rej)=>{this.p.set(id,{res,rej});this.ws.send(JSON.stringify({id,method,params,sessionId}));});}
  async ev(S,e,a=false){const r=await this.send("Runtime.evaluate",{expression:e,awaitPromise:a,returnByValue:true},S);if(r.exceptionDetails)throw new Error(r.exceptionDetails.text);return r.result.value;} }

const server = spawn(process.execPath, [new URL("./serve.mjs", import.meta.url).pathname, String(PORT)], { stdio: ["ignore", "pipe", "inherit"] });
await sleep(400);
const chrome = spawn(EXE, ["--headless=new", "--remote-debugging-port=9347", "--remote-allow-origins=*", "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--hide-scrollbars", "--force-device-scale-factor=2", "--window-size=1280,1120", `--user-data-dir=/tmp/lk-shot-${process.pid}`, "about:blank"], { stdio: ["ignore", "pipe", "pipe"] });
const wsUrl = await new Promise((res, rej) => { let b = ""; const to = setTimeout(() => rej(new Error("timeout")), 15000); chrome.stderr.on("data", d => { b += d; const m = b.match(/ws:\/\/[^\s]+/); if (m) { clearTimeout(to); res(m[0]); } }); });

const cdp = new CDP(wsUrl); await cdp.open;
const { targetId } = await cdp.send("Target.createTarget", { url: PAGE });
const { sessionId: S } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
await cdp.send("Runtime.enable", {}, S);
await cdp.send("Emulation.setDeviceMetricsOverride", { width: 1280, height: 1120, deviceScaleFactor: 2, mobile: false }, S);
for (let i = 0; i < 100; i++) { if (await cdp.ev(S, "!!window.__lkm?.ready")) break; await sleep(100); }
// LushPad showcase (10 nodes: svf → chorus → reverb) + riff so the signal-flow
// graph lights up, plus a cutoff scrub so the badge + receipts are populated.
await cdp.ev(S, `(async()=>{const {EXAMPLES}=await import('./lk-dsl.mjs'); await window.__lkm.setPatchText(EXAMPLES.LushPad); return true;})()`, true).catch(() => {});
await cdp.ev(S, "window.__lkm.latch(true)");
await sleep(150);
await cdp.ev(S, "window.__lkm.riff(true)");
await sleep(800);
for (const hz of [0.9, 1.6, 0.7, 1.4]) { await cdp.ev(S, `window.__lkm.editNumber('cutoff', ${hz}, 'khz')`, true); await sleep(160); }
await sleep(700);
const { data } = await cdp.send("Page.captureScreenshot", { format: "png", captureBeyondViewport: false }, S);
writeFileSync(out, Buffer.from(data, "base64"));
console.log("wrote " + out);
try { chrome.kill("SIGTERM"); } catch {}
server.kill("SIGTERM");
await sleep(150);
process.exit(0);
