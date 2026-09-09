// Browser integration tests with a local mock ESP32 API; no physical device writes.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import {pathToFileURL} from 'node:url';

const htmlPath=path.resolve(process.argv[2]);
const packageRoot=process.argv[3];
const {chromium}=packageRoot?await import(pathToFileURL(path.join(packageRoot,'playwright/index.mjs'))):await import('playwright');
const html=fs.readFileSync(htmlPath,'utf8');
const output=path.resolve(path.dirname(htmlPath),'../build-page-tests');
fs.mkdirSync(output,{recursive:true});
const target={type:'Feature',properties:{name:'Target',role:'Target',source:'manual-placement',mapProvider:'AMap',coordinateSystem:'WGS84',sourceCoordinateSystem:'GCJ-02',coordinateConversion:'approximate GCJ-02 to WGS84'},geometry:{type:'Point',coordinates:[116.3374289,39.9762859]}};
const fence=JSON.parse(fs.readFileSync(path.resolve(path.dirname(htmlPath),'../docs/examples/beihang-fence.geojson'),'utf8'));
const empty={type:'Feature',properties:{name:'',enabled:false,coordinateSystem:'WGS84'},geometry:null};
const paths=['/api/v1/target','/api/v1/fence/1','/api/v1/fence/2'];
let records={[paths[0]]:structuredClone(target),[paths[1]]:structuredClone(empty),[paths[2]]:structuredClone(empty)};
const writes=[];let failure='',delayTarget=0;
const server=http.createServer(async(req,res)=>{
  if(req.url==='/'){res.setHeader('Content-Type','text/html; charset=utf-8');res.setHeader('Content-Security-Policy',"default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; object-src 'none'");res.end(html);return;}
  const send=(status,data)=>{res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(data));};
  if(req.url==='/api/v1/status'){send(200,{ok:true,ssid:'MCCompass-2460',gpsValid:true,revision:1,fences:[{id:'fence1',state:'inside'},{id:'fence2',state:'unconfigured'}]});return;}
  if(!paths.includes(req.url)){send(404,{});return;}
  if(req.method==='GET'){
    if(req.url===paths[0]&&delayTarget)await new Promise(resolve=>setTimeout(resolve,delayTarget));
    if(failure==='legacy'&&req.url===paths[1]){send(409,{ok:false,error:{code:'LEGACY_CRS'}});return;}
    send(200,records[req.url]);return;
  }
  let raw='';for await(const chunk of req)raw+=chunk;
  writes.push({url:req.url,raw,contentType:req.headers['content-type']});
  if(failure==='disconnect'){req.socket.destroy();return;}
  if(failure==='nvs'||failure==='duplicate'){
    send(failure==='nvs'?503:422,{ok:false,error:{code:failure==='nvs'?'NVS_WRITE':'JSON_DUPLICATE_FIELD'}});return;
  }
  const value=JSON.parse(raw);
  records[req.url]=value;
  send(200,value);
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const base=`http://127.0.0.1:${server.address().port}`;
const executable=process.env.COMPASS_BROWSER||'C:/Program Files/Google/Chrome/Application/chrome.exe';
let browser;
try{
  browser=await chromium.launch({headless:true,executablePath:executable});
  const page=await browser.newPage({viewport:{width:390,height:844},isMobile:true,hasTouch:true,deviceScaleFactor:1});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  const waitText=async(selector,text)=>{await page.waitForFunction(({selector,text})=>document.querySelector(selector).textContent.includes(text),{selector,text});};
  const ready=async()=>{await page.goto(base);await waitText('#fence2-current','已禁用');};
  const noOverflow=async()=>assert(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth),'horizontal overflow');
  await ready();
  assert.equal(await page.locator('[id^="panel-fence"] input').count(),0,'fence manual inputs removed');
  assert.equal(await page.locator('#panel-target:visible').count(),1);
  assert.equal(await page.locator('#fence1-json').inputValue(),'');
  assert.equal(await page.locator('#target-lon').inputValue(),'116.3374289');
  await page.screenshot({path:path.join(output,'target-mobile.png'),fullPage:true});
  const sendManual=()=>page.getByRole('button',{name:'应用目标坐标',exact:true}).click();
  for(const bad of ['', '181', 'NaN', '0x12']){
    await page.locator('#target-lon').fill(bad);const count=writes.length;await sendManual();
    await waitText('#target-message','合法经度');assert.equal(writes.length,count);
  }
  await page.locator('#target-lon').fill('116.3374289');await page.locator('#target-lat').fill('91');
  await sendManual();await waitText('#target-message','合法纬度');
  await page.locator('#target-name').fill('手机目标');await page.locator('#target-lat').fill('39.9762859');
  await sendManual();await waitText('#target-message','已应用');
  assert.deepEqual(JSON.parse(writes.at(-1).raw).geometry.coordinates,[116.3374289,39.9762859]);
  assert.equal(writes.at(-1).contentType,'application/geo+json');
  await page.getByRole('tab',{name:'粘贴 GeoJSON',exact:true}).click();
  const targetRaw=JSON.stringify(target,null,2);await page.locator('#target-json').fill(targetRaw);
  await page.getByRole('button',{name:'应用 Target GeoJSON',exact:true}).click();await waitText('#target-message','已应用');
  assert.equal(writes.at(-1).raw,targetRaw,'source metadata and raw JSON retained');
  await page.getByRole('tab',{name:'围栏 1',exact:true}).click();
  const fenceRaw=JSON.stringify(fence,null,2);await page.locator('#fence1-json').fill(fenceRaw);
  await waitText('#fence1-preview-text','7 个顶点');
  await page.locator('#fence1-shape').waitFor({state:'visible'});
  for(const width of [320,375,390,768,1440]){await page.setViewportSize({width,height:844});await noOverflow();}
  await page.setViewportSize({width:390,height:844});
  await page.screenshot({path:path.join(output,'fence-mobile.png'),fullPage:true});
  await page.getByRole('button',{name:'应用围栏 1',exact:true}).click();await waitText('#fence1-message','已应用');
  assert.equal(writes.at(-1).url,paths[1]);assert.equal(writes.at(-1).raw,fenceRaw);
  assert.deepEqual(records[paths[2]],empty,'fence 1 save does not change fence 2');
  const badFences=['{}','{',' '.repeat(3073)+fenceRaw,JSON.stringify({...fence,geometry:{type:'MultiPolygon',coordinates:[]}}),JSON.stringify({...fence,geometry:{type:'Polygon',coordinates:[fence.geometry.coordinates[0].slice(0,-1)]}}),JSON.stringify({...fence,geometry:{type:'Polygon',coordinates:[...fence.geometry.coordinates,...fence.geometry.coordinates]}}),JSON.stringify({...fence,properties:{coordinateSystem:'GCJ-02'}})];
  for(const raw of badFences){const count=writes.length;await page.locator('#fence1-json').fill(raw);await page.getByRole('button',{name:'应用围栏 1',exact:true}).click();await page.locator('#fence1-message.error').waitFor();assert.equal(writes.length,count,'invalid input must not be sent');}
  failure='nvs';await page.locator('#fence1-json').fill(fenceRaw);await page.getByRole('button',{name:'应用围栏 1',exact:true}).click();await waitText('#fence1-message','原配置继续生效');
  assert(!(await page.locator('[data-save="fence1"]').isDisabled()));
  failure='duplicate';const duplicate=fenceRaw.replace('"type": "Feature"','"type": "Feature", "type": "Feature"');
  await page.locator('#fence1-json').fill(duplicate);await page.getByRole('button',{name:'应用围栏 1',exact:true}).click();await waitText('#fence1-message','重复字段');assert.equal(writes.at(-1).raw,duplicate);
  failure='disconnect';await page.locator('#fence1-json').fill(fenceRaw);await page.getByRole('button',{name:'应用围栏 1',exact:true}).click();await waitText('#fence1-message','应用结果未确认');assert(!(await page.locator('#fence1-json').isDisabled()));
  failure='';await page.getByRole('tab',{name:'围栏 2',exact:true}).click();await page.locator('#fence2-json').fill(fenceRaw);await page.getByRole('button',{name:'应用围栏 2',exact:true}).click();await waitText('#fence2-message','已应用');assert.equal(writes.at(-1).url,paths[2]);
  const disabled={type:'Feature',properties:{enabled:false},geometry:null};
  await page.locator('#fence2-json').fill(JSON.stringify(disabled));await page.getByRole('button',{name:'应用围栏 2',exact:true}).click();await waitText('#fence2-message','已应用');await waitText('#fence2-current','已禁用');
  failure='legacy';await page.reload();await waitText('#fence1-message','旧 GCJ02');
  failure='';delayTarget=650;await page.reload();await page.locator('#target-lon').fill('120.123');await waitText('#target-current','Target');assert.equal(await page.locator('#target-lon').inputValue(),'120.123','late GET must preserve draft');delayTarget=0;
  assert.deepEqual(errors,[]);
  console.log('PASS: mobile layout (320–1440px), manual/GeoJSON Target, 2 Polygon slots, original user samples, invalid input, raw duplicate fields, server failure, disconnect, legacy CRS, delayed-load draft protection.');
  console.log(`Screenshots: ${output}`);
}finally{if(browser)await browser.close();server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
