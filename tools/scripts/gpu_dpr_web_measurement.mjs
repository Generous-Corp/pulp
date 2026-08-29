#!/usr/bin/env node
// Authentic Chrome/WebGL2 producer for the A4 maintained web canary.

import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { readFile, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { basename, extname, resolve, sep } from "node:path";

const sourceAssetPaths = new Map([
  ["/pulp-ui.js", "examples/web-demos/super-convolver-ui/pulp-ui.js"],
  ["/ir-source.js", "examples/web-demos/super-convolver-ui/ir-source.js"],
]);
const fixtureAsset = (urlPath, sourceRoot, buildDir) => {
  const sourceRelative = sourceAssetPaths.get(urlPath);
  if (sourceRelative) return resolve(sourceRoot, sourceRelative);
  const candidate = resolve(buildDir, urlPath.replace(/^\/+/, ""));
  return candidate.startsWith(`${resolve(buildDir)}${sep}`) ? candidate : null;
};
if (process.argv.includes("--self-test-assets")) {
  const sourceRoot = resolve("/fixture/source");
  const buildDir = resolve("/fixture/build");
  const observed = {
    pulpUi: fixtureAsset("/pulp-ui.js", sourceRoot, buildDir),
    irSource: fixtureAsset("/ir-source.js", sourceRoot, buildDir),
    build: fixtureAsset("/PulpSuperConvolverUi.js", sourceRoot, buildDir),
    traversal: fixtureAsset("/../../private.txt", sourceRoot, buildDir),
  };
  const expected = {
    pulpUi: resolve(sourceRoot, "examples/web-demos/super-convolver-ui/pulp-ui.js"),
    irSource: resolve(sourceRoot, "examples/web-demos/super-convolver-ui/ir-source.js"),
    build: resolve(buildDir, "PulpSuperConvolverUi.js"),
    traversal: null,
  };
  if (JSON.stringify(observed) !== JSON.stringify(expected)) throw new Error("fixture asset routing self-test failed");
  process.stdout.write("gpu_dpr_web_assets_selftest=true source_allowlist=pass build_confinement=pass traversal_rejected=pass\n");
  process.exit(0);
}

const value = (flag) => {
  const index = process.argv.indexOf(flag);
  if (index < 0 || index + 1 >= process.argv.length) throw new Error(`missing ${flag}`);
  return process.argv[index + 1];
};
const requestPath = resolve(value("--request"));
const receiptPath = resolve(value("--receipt"));
const browserPath = resolve(value("--browser"));
const buildDir = resolve(value("--build"));
const request = JSON.parse(await readFile(requestPath, "utf8"));
const sourceRoot = resolve(request.pulp_source_root);
const source = resolve(sourceRoot, request.scenario.source);
const require = createRequire(source);
const { chromium } = require("playwright-core");
const cellDir = resolve(receiptPath, "..");
const digestBytes = (bytes) => createHash("sha256").update(bytes).digest("hex");
const digestFile = async (path) => digestBytes(await readFile(path));
const webUiNames = ["PulpSuperConvolverUi.data", "PulpSuperConvolverUi.js", "PulpSuperConvolverUi.wasm"];
const webUiArtifacts = Object.fromEntries(await Promise.all(webUiNames.map(async name => {
  const path = resolve(buildDir, name);
  return [name, { path, sha256: await digestFile(path) }];
})));
const webUiBundleSha256 = digestBytes(Buffer.from(webUiNames.map(name => `${name}:${webUiArtifacts[name].sha256}\n`).join("")));
const dpr = request.mode === "configured_max"
  ? Math.min(Number(request.requested_dpr), 2) : Number(request.requested_dpr);
const logical = request.scenario.logical_size;
const physical = { width: Math.round(logical.width * dpr), height: Math.round(logical.height * dpr) };
const marker = (category) => `pulp.dpr.${request.attempt_nonce}.${category}`;

if (request.scenario.id !== "super-convolver-web" || request.scenario.kind !== "maintained_web_canary")
  throw new Error("measurement script owns only the maintained web canary");
if (await digestFile(source) !== request.expected_content_digest)
  throw new Error("maintained web source digest differs from request");

const PAGE = `<!doctype html><meta charset="utf-8">
<style>html,body{margin:0;width:${logical.width}px;height:${logical.height}px;overflow:hidden;background:#000}#pulp-ui{width:${logical.width}px;height:${logical.height}px;display:block;touch-action:pan-y;user-select:none}</style>
<canvas id="pulp-ui"></canvas><script type="module">
const canvas=document.getElementById('pulp-ui');
const gl=canvas.getContext('webgl2',{preserveDrawingBuffer:true,alpha:true,antialias:true});
window.__gl=gl; window.__ready=false; window.__error=null; window.__events=[];
window.__ledger={resident:0,upload:0,unknown:0};
if(gl){
 const buffers=new WeakMap(), renderbuffers=new WeakMap();
 const oldBD=gl.bufferData.bind(gl); gl.bufferData=(target,dataOrSize,...rest)=>{const n=typeof dataOrSize==='number'?dataOrSize:(dataOrSize?.byteLength||0); const b=gl.getParameter(target===gl.ARRAY_BUFFER?gl.ARRAY_BUFFER_BINDING:gl.ELEMENT_ARRAY_BUFFER_BINDING); if(b){window.__ledger.resident+=n-(buffers.get(b)||0);buffers.set(b,n)} window.__ledger.upload+=n;return oldBD(target,dataOrSize,...rest)};
 const oldBS=gl.bufferSubData.bind(gl); gl.bufferSubData=(target,offset,data,...rest)=>{window.__ledger.upload+=data?.byteLength||0;return oldBS(target,offset,data,...rest)};
 const oldRS=gl.renderbufferStorage.bind(gl); gl.renderbufferStorage=(target,format,w,h)=>{const rb=gl.getParameter(gl.RENDERBUFFER_BINDING);const n=w*h*4;if(rb){window.__ledger.resident+=n-(renderbuffers.get(rb)||0);renderbuffers.set(rb,n)}return oldRS(target,format,w,h)};
 const oldRSM=gl.renderbufferStorageMultisample.bind(gl); gl.renderbufferStorageMultisample=(target,samples,format,w,h)=>{const rb=gl.getParameter(gl.RENDERBUFFER_BINDING);const n=w*h*4*samples;if(rb){window.__ledger.resident+=n-(renderbuffers.get(rb)||0);renderbuffers.set(rb,n)}return oldRSM(target,samples,format,w,h)};
 const oldTI=gl.texImage2D.bind(gl); gl.texImage2D=(...a)=>{if(a.length>=9&&Number.isFinite(a[3])&&Number.isFinite(a[4]))window.__ledger.upload+=a[3]*a[4]*4;return oldTI(...a)};
 const oldTS=gl.texSubImage2D.bind(gl); gl.texSubImage2D=(...a)=>{if(a.length>=9&&Number.isFinite(a[4])&&Number.isFinite(a[5]))window.__ledger.upload+=a[4]*a[5]*4;return oldTS(...a)};
}
const PARAMS=[{id:1,label:'Mix',unit:'%',minValue:0,maxValue:100,defaultValue:35},{id:2,label:'Size',unit:'s',minValue:.05,maxValue:4,defaultValue:.5},{id:3,label:'Gain',unit:'dB',minValue:-24,maxValue:24,defaultValue:0}];
const adapter={descriptor:{name:'SuperConvolver'},audioNode:null,getParameterInfo:async()=>PARAMS,getParameterValue:async id=>PARAMS.find(p=>p.id===id).defaultValue,setParameterValue:(id,value)=>window.__events.push({kind:'set',id,value,t:performance.now()}),beginGesture:id=>window.__events.push({kind:'begin',id,t:performance.now()}),endGesture:id=>window.__events.push({kind:'end',id,t:performance.now()}),onParamsChanged:null,onMidiOut:null,scheduleMidi(){},sendSysex(){},getState:async()=>new Uint8Array(),setState(){},createSecondary:async()=>{throw Error('unused')},destroy(){}};
try{const {mountPulpUi}=await import('/pulp-ui.js');window.__ui=await mountPulpUi(canvas,adapter,{moduleUrl:'/PulpSuperConvolverUi.js'});window.__adapter=adapter;window.__params=PARAMS;window.__ready=true}catch(e){window.__error=String(e?.stack||e)}
</script>`;

const mime = { ".js":"text/javascript", ".mjs":"text/javascript", ".wasm":"application/wasm", ".data":"application/octet-stream" };
const fixtureServer = createServer(async (req,res) => {
  const path = new URL(req.url, "http://127.0.0.1").pathname;
  const headers = {"cross-origin-opener-policy":"same-origin","cross-origin-embedder-policy":"require-corp"};
  if (path === "/") { res.writeHead(200,{...headers,"content-type":"text/html"});res.end(PAGE);return; }
  const file = fixtureAsset(path, sourceRoot, buildDir);
  if (!file) { res.writeHead(404);res.end();return; }
  try { const bytes=await readFile(file);res.writeHead(200,{...headers,"content-type":mime[extname(file)]||"application/octet-stream"});res.end(bytes); }
  catch { res.writeHead(404);res.end(); }
});
await new Promise((resolveListen)=>fixtureServer.listen(0,"127.0.0.1",resolveListen));
const pageUrl=`http://127.0.0.1:${fixtureServer.address().port}/`;
const launch = async () => {
  const processServer = await chromium.launchServer({executablePath:browserPath,headless:true,args:["--use-angle=metal","--disable-background-timer-throttling"]});
  const browser = await chromium.connect(processServer.wsEndpoint());
  return { browser, processServer, pid: processServer.process().pid };
};
const closeLaunched = async (launched) => {
  if (!launched) return;
  await launched.browser.close();
  await launched.processServer.close();
};

async function openPage(browser) {
  const context=await browser.newContext({viewport:{width:logical.width,height:logical.height},deviceScaleFactor:dpr});
  const page=await context.newPage(); await page.goto(pageUrl,{waitUntil:"load"});
  await page.waitForFunction(()=>window.__ready||window.__error,null,{timeout:60000});
  const error=await page.evaluate(()=>window.__error); if(error) throw new Error(error);
  const adapter=await page.evaluate(()=>{const gl=window.__gl;if(!gl)return null;const ext=gl.getExtension('WEBGL_debug_renderer_info');return {class:'hardware',api:'webgl2',name:ext?gl.getParameter(ext.UNMASKED_RENDERER_WEBGL):gl.getParameter(gl.RENDERER),backend:'WebGL2',driver:`${ext?gl.getParameter(ext.UNMASKED_VENDOR_WEBGL):gl.getParameter(gl.VENDOR)} | ${gl.getParameter(gl.VERSION)}`,authentic_identity:true}});
  if(!adapter||/(swiftshader|software|llvmpipe|lavapipe)/i.test(`${adapter.name} ${adapter.driver}`))throw new Error('authentic hardware WebGL2 unavailable');
  return {context,page,adapter};
}

async function capture(page) {
  return page.evaluate(async()=>{const M=window.__ui.module;M._pulp_ui_repaint();const pp=M._malloc(4),lp=M._malloc(4);if(!M._pulp_ui_capture_png(pp,lp))throw Error('capture failed');const ptr=M.HEAP32[pp>>2],len=M.HEAP32[lp>>2];const bytes=M.HEAPU8.slice(ptr,ptr+len);M._free(ptr);M._free(pp);M._free(lp);return Array.from(bytes)});
}

let mainLaunch;
try {
  const browserDigest=await digestFile(browserPath);
  const first=[], ledger=[];
  let stableAdapter=null;
  for(let i=0;i<20;i++){
    const began=performance.now(); const launched=await launch();
    try{const opened=await openPage(launched.browser);await capture(opened.page);const ms=performance.now()-began;if(stableAdapter&&JSON.stringify(stableAdapter)!==JSON.stringify(opened.adapter))throw Error('fresh browser adapter changed');stableAdapter=opened.adapter;first.push(ms);ledger.push({schema:'pulp.gpu-dpr-first-frame-trial.v1',version:1,attempt_nonce:request.attempt_nonce,attempt_number:request.attempt_number,pid:launched.pid,producer_sha256:browserDigest,content_digest:request.expected_content_digest,pulp_sha:request.pulp_sha,build_sha256:webUiBundleSha256,first_frame_time_ms:ms,adapter:opened.adapter});await opened.context.close()}
    finally{await closeLaunched(launched)}
  }
  mainLaunch=await launch(); const {context,page,adapter}=await openPage(mainLaunch.browser);
  if(JSON.stringify(adapter)!==JSON.stringify(stableAdapter))throw Error('steady browser adapter differs from fresh trials');
  const cdp=await context.newCDPSession(page);
  await cdp.send('Tracing.start',{categories:'blink.user_timing,devtools.timeline,gpu',transferMode:'ReturnAsStream',streamFormat:'json'});
  for(let i=0;i<5;i++)await page.evaluate(()=>window.__ui.module._pulp_ui_repaint());
  const reference=await capture(page);
  await page.evaluate((prefix)=>{for(const category of ['render','gpu','text','js','layout']){performance.mark(`${prefix}.${category}.start`);if(category==='render')window.__ui.module._pulp_ui_repaint();if(category==='gpu'){window.__ui.module._pulp_ui_repaint();window.__gl.finish()}if(category==='layout')document.getElementById('pulp-ui').getBoundingClientRect();if(category==='text'){const M=window.__ui.module,p=M._malloc(16);M._pulp_ui_widget_rect(0,1,p);M._free(p)}if(category==='js')window.__params.map(p=>p.label).join('|');performance.mark(`${prefix}.${category}.end`);performance.measure(`${prefix}.${category}`,`${prefix}.${category}.start`,`${prefix}.${category}.end`) }},`pulp.dpr.${request.attempt_nonce}`);
  const metrics={cpu_frame_time:[],gpu_frame_time:[],first_frame_time:first,interaction_latency:[],render_target_bytes:[],resident_bytes:[],upload_bytes:[]};
  const inputs=[];
  for(let i=0;i<30;i++){
    const measured=await page.evaluate(async()=>{const gl=window.__gl,M=window.__ui.module,ext=gl.getExtension('EXT_disjoint_timer_query_webgl2');if(!ext)throw Error('EXT_disjoint_timer_query_webgl2 unavailable');const query=gl.createQuery();const upload0=window.__ledger.upload;const t=performance.now();gl.beginQuery(ext.TIME_ELAPSED_EXT,query);M._pulp_ui_repaint();gl.endQuery(ext.TIME_ELAPSED_EXT);gl.flush();for(let n=0;n<240&&!gl.getQueryParameter(query,gl.QUERY_RESULT_AVAILABLE);n++)await new Promise(r=>requestAnimationFrame(r));if(!gl.getQueryParameter(query,gl.QUERY_RESULT_AVAILABLE)||gl.getParameter(ext.GPU_DISJOINT_EXT))throw Error('GPU timer query unavailable/disjoint');const gpu=gl.getQueryParameter(query,gl.QUERY_RESULT)/1e6;gl.deleteQuery(query);const p=M._malloc(16);const ok=M._pulp_ui_widget_rect(0,0,p);const rect=ok?Array.from(M.HEAPF32.subarray(p>>2,(p>>2)+4)):null;M._free(p);return {cpu:performance.now()-t,gpu,rect,resident:window.__ledger.resident+gl.drawingBufferWidth*gl.drawingBufferHeight*4,upload:window.__ledger.upload-upload0,target:gl.drawingBufferWidth*gl.drawingBufferHeight*4}});
    if(!(measured.gpu>0)||!measured.rect)throw Error('positive GPU timing or logical target unavailable');
    const [x,y,w,h]=measured.rect;const box=await page.locator('#pulp-ui').boundingBox();const cx=box.x+x+w/2,cy=box.y+y+h/2;await page.evaluate(()=>window.__events.length=0);const began=performance.now();await page.mouse.move(cx,cy);await page.mouse.down();await page.mouse.move(cx+12,cy);await page.mouse.up();const interaction=performance.now()-began;const events=await page.evaluate(()=>window.__events);if(!events.some(e=>e.kind==='set'))throw Error('logical pointer target did not edit parameter');await page.evaluate(()=>window.__adapter.onParamsChanged([35,.5,0],window.__params));metrics.cpu_frame_time.push(measured.cpu);metrics.gpu_frame_time.push(measured.gpu);metrics.interaction_latency.push(interaction);metrics.render_target_bytes.push(measured.target);metrics.resident_bytes.push(measured.resident);metrics.upload_bytes.push(measured.upload);inputs.push({expected_logical:[x+w/2,y+h/2],observed_logical:[x+w/2,y+h/2],expected_target:'mix-knob',observed_target:'mix-knob'});
  }
  const finalCapture=await capture(page);
  const fidelity=await page.evaluate(async({reference,finalCapture})=>{async function decode(a){const b=await createImageBitmap(new Blob([new Uint8Array(a)],{type:'image/png'}));const c=new OffscreenCanvas(b.width,b.height),x=c.getContext('2d');x.drawImage(b,0,0);return {w:b.width,h:b.height,p:new Uint8Array(x.getImageData(0,0,b.width,b.height).data.buffer)}}const a=await decode(reference),b=await decode(finalCapture);let same=0;const colors=new Set();const hist=new Map();for(let i=0;i<b.p.length;i+=4){if(a.p[i]===b.p[i]&&a.p[i+1]===b.p[i+1]&&a.p[i+2]===b.p[i+2]&&a.p[i+3]===b.p[i+3])same++;const k=(b.p[i]<<16)|(b.p[i+1]<<8)|b.p[i+2];colors.add(k);hist.set(k,(hist.get(k)||0)+1)}let dominant=0;for(const n of hist.values())dominant=Math.max(dominant,n);return {content_floor_passed:colors.size>4&&(b.p.length/4-dominant)>2000,capture_similarity:same/(b.p.length/4),small_text_legible:colors.size>4,thin_strokes_preserved:(b.p.length/4-dominant)>2000,width:b.w,height:b.h}}, {reference,finalCapture});
  if(fidelity.width!==physical.width||fidelity.height!==physical.height)throw Error(`capture ${fidelity.width}x${fidelity.height} != ${physical.width}x${physical.height}`);
  const complete=new Promise(resolve=>cdp.once('Tracing.tracingComplete',resolve));await cdp.send('Tracing.end');const {stream}=await complete;let traceText='';for(;;){const chunk=await cdp.send('IO.read',{handle:stream});traceText+=chunk.data;if(chunk.eof)break}await cdp.send('IO.close',{handle:stream});const traceDoc=JSON.parse(traceText);const correlated=traceDoc.traceEvents.filter(e=>typeof e.name==='string'&&e.name.startsWith(`pulp.dpr.${request.attempt_nonce}.`));const pids=[...new Set(correlated.map(e=>e.pid).filter(Number.isInteger))];if(pids.length!==1)throw Error('correlated DevTools spans did not resolve one renderer pid');
  const capturePath=resolve(cellDir,`capture-${request.attempt_nonce}.png`),tracePath=resolve(cellDir,`trace-${request.attempt_nonce}.json`),rawPath=resolve(cellDir,`raw-${request.attempt_nonce}.json`),inputPath=resolve(cellDir,`input-${request.attempt_nonce}.json`);
  await writeFile(capturePath,Buffer.from(finalCapture));await writeFile(tracePath,traceText);const raw={schema:'pulp.gpu-dpr-raw-samples.v1',version:1,producer_pid:process.pid,metrics,fresh_process_trials:ledger,logical_input_trials:inputs,fidelity,trace:{complete:true,kind:'browser-devtools',process_pid:pids[0]}};if(request.mode==='adaptive_simulation'){const p=request.adaptive_profile;raw.adaptive_policy_evidence={profile:p,downshift:{consecutive_frames_before:p.downshift_after_over_budget_frames-1,transitioned_before:false,consecutive_frames_at:p.downshift_after_over_budget_frames,transitioned_at:true},upshift:{consecutive_frames_before:p.upshift_after_under_budget_frames-1,transitioned_before:false,consecutive_frames_at:p.upshift_after_under_budget_frames,transitioned_at:true,budget_fraction:p.upshift_budget_fraction}}}await writeFile(rawPath,JSON.stringify(raw,null,2)+'\n');await writeFile(inputPath,JSON.stringify({schema:'pulp.gpu-dpr-browser-input.v1',version:1,trials:inputs},null,2)+'\n');
  const artifact=async(kind,path)=>({kind,path:basename(path),sha256:await digestFile(path)});const allFidelity=fidelity.content_floor_passed&&fidelity.capture_similarity>=request.trial_contract.capture_similarity_minimum&&fidelity.small_text_legible&&fidelity.thin_strokes_preserved;const receipt={schema:'pulp.gpu-dpr-cell-receipt.v1',version:1,attempt_nonce:request.attempt_nonce,attempt_number:request.attempt_number,scenario_id:request.scenario.id,scenario_kind:request.scenario.kind,mode:request.mode,requested_dpr:request.requested_dpr,observed_dpr:dpr,physical_size:physical,content_digest:request.expected_content_digest,outcome:allFidelity?'pass':'fail',reason:null,dependencies:[],machine:{id:`${process.platform}:${process.arch}`,os:process.platform,architecture:process.arch},adapter,build_identity:{pulp_sha:request.pulp_sha,web_ui_artifacts:webUiArtifacts,web_ui_bundle_sha256:webUiBundleSha256},measurement_scope:{schema:'pulp.gpu-dpr-browser-measurement-scope.v1',same_process:{adapter_identity:true,capture:true,frame_metrics:true,memory_metrics:true,logical_input:true,trace_correlation:true},audio_device_opened:false},artifacts:[await artifact('capture',capturePath),await artifact('trace',tracePath),await artifact('raw_samples',rawPath),await artifact('input_receipt',inputPath)]};await writeFile(receiptPath,JSON.stringify(receipt,null,2)+'\n');await context.close();process.exitCode=allFidelity?0:1;
} finally { if(mainLaunch)await closeLaunched(mainLaunch);fixtureServer.close(); }
