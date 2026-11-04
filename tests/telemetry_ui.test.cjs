// Run with Playwright installed, or RID_PLAYWRIGHT_MODULE pointing to playwright-core.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {chromium}=require(process.env.RID_PLAYWRIGHT_MODULE||'playwright');
const root=path.join(__dirname,'../RID_Workbench');
const raw=(file,tag)=>fs.readFileSync(path.join(root,file),'utf8').split(`R"${tag}(`)[1].split(`)${tag}";`)[0];
const assets={'/':raw('page.h','HTML'),'/navigation.js':raw('navigation.h','JS'),'/map-picker.js':raw('map_picker.h','JS')};
const profile={horizontalAccuracyM:10,verticalAccuracyM:3,speedAccuracyMps:1,timeAccuracyS:0.1};
function fixture(){return {devices:0,packets:0,items:[],monitoring:false,scanning:false,wifiListening:false,bleReports:0,wifiReports:0,dropped:0,
 tx:{enabled:false,timeSynced:false,mode:'single',uasId:'TEST-SAVED',opId:'TEST-OPERATOR',lat:31.23,lon:121.47,alt:70,opLat:31.23,opLon:121.47,opAlt:10,duration:60,height:60,heading:90,speed:12.5,verticalSpeed:0.5,telemetryProfile:profile,simulation:{count:100,seed:1234,wifi:true,ble:true}},
 radio:{wifiQuarterDbm:8,bleDbm:-12,wifiActualDbm:2,bleActualDbm:-12,wifiOptions:[{value:8,dbm:2}],bleOptions:[{value:-12,dbm:-12}],wifiScan:1,bleScan:1,rssiFloor:-127},led:{mode:'off',pin:48}};}
function roster(){return {count:100,seed:1234,centerLat:31.23,centerLon:121.47,modelIndex:-1,models:['mini5pro'],elapsedMs:0,telemetryProfile:profile,timeSynced:false,items:Array.from({length:100},(_,i)=>({sn:`SIM-${i}`,model:'mini5pro',lat:31.23+i*0.0001,lon:121.47,alt:70,height:60,speed:12.5,verticalSpeed:0.5,heading:90,opLat:31.23,opLon:121.47,opAlt:10,takeoffLat:31.23,takeoffLon:121.47,takeoffAlt:10}))};}
(async()=>{
 const browser=await chromium.launch({headless:true,...(process.env.RID_CHROME_PATH?{executablePath:process.env.RID_CHROME_PATH}:{})});
 try{
  for(const mobile of [false,true]){
   const context=await browser.newContext({viewport:mobile?{width:390,height:844}:{width:1440,height:1000},isMobile:mobile,hasTouch:mobile});
   const page=await context.newPage(),errors=[],requests=[],state=fixture();page.on('pageerror',e=>errors.push(e.message));
   await context.route('**/*',async route=>{
    const url=new URL(route.request().url());if(url.hostname!=='rid.test')return route.abort();
    if(Object.hasOwn(assets,url.pathname))return route.fulfill({body:assets[url.pathname],contentType:url.pathname==='/'?'text/html':'application/javascript'});
    const body=Object.fromEntries(new URLSearchParams(route.request().postData()||''));requests.push({path:url.pathname,body});
    if(url.pathname==='/api/config')return route.fulfill({json:{ssid:'RID-Test-ESP32',open:true}});
    if(url.pathname==='/api/simulation')return route.fulfill({json:roster()});
    if(url.pathname==='/api/tx/save'||url.pathname==='/api/tx/start'){
     for(const [k,v] of Object.entries(body))if(k!=='utcMs')state.tx[k]=['uasId','opId'].includes(k)?v:Number(v);
     if(url.pathname.endsWith('/start'))Object.assign(state.tx,{enabled:true,timeSynced:!!body.utcMs,mode:'single',remaining:60,ble:true,wifi:true});
    }
    if(url.pathname==='/api/simulation/start')Object.assign(state.tx,{enabled:true,timeSynced:!!body.utcMs,mode:'simulation',remaining:60});
    if(url.pathname==='/api/tx/stop')state.tx.enabled=false;
    return route.fulfill({json:state});
   });
   await page.goto('http://rid.test/');await page.waitForFunction(()=>document.querySelector('#link').textContent==='设备已连接');
   await page.locator('[data-tab="transmit"]').click();
   const expected={height:'86.5',heading:'270',speed:'15.25',verticalSpeed:'-1.5'};
   for(const [key,value] of Object.entries(expected))await page.locator('#'+key).fill(value);
   assert.match(await page.locator('#previewMotion').textContent(),/86.5 m · 270°/);
   assert.match(await page.locator('#previewSpeed').textContent(),/15.25 \/ -1.5 m\/s/);
   await page.locator('#saveTx').click();await page.waitForFunction(()=>document.querySelector('#toast').textContent==='参数已保存，未开启发射');
   const saved=requests.findLast(x=>x.path==='/api/tx/save');for(const [k,v] of Object.entries(expected))assert.equal(saved.body[k],v);assert.equal(saved.body.utcMs,undefined);assert.equal(state.tx.enabled,false);
   const before=Date.now();await page.locator('#startTx').click();await page.waitForFunction(()=>document.querySelector('#actualTx').textContent==='BLE + Wi-Fi');
   const sent=requests.findLast(x=>x.path==='/api/tx/start');assert(Number(sent.body.utcMs)>=before && Number(sent.body.utcMs)<=Date.now());
   for(const [k,v] of Object.entries(expected))assert.equal(sent.body[k],v);
   assert.match(await page.locator('#singleClock').textContent(),/已校时/);
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
   if(process.env.RID_SCREENSHOT_DIR){fs.mkdirSync(process.env.RID_SCREENSHOT_DIR,{recursive:true});await page.screenshot({path:path.join(process.env.RID_SCREENSHOT_DIR,mobile?'single-mobile.png':'single-desktop.png'),fullPage:true});}
   await page.locator('#globalStop').click();await page.waitForFunction(()=>document.querySelector('#liveStrip').hidden);
   await page.locator('[data-txmode="simulation"]').click();await page.waitForFunction(()=>!document.querySelector('#startSim').disabled);
   await page.locator('.sim-roster summary').click();assert.equal(await page.locator('#simRoster tr').count(),100);
   assert.match(await page.locator('#simRoster tr').first().textContent(),/相对起飞点 60.0 m/);
   const simBefore=Date.now();await page.locator('#startSim').click();await page.waitForFunction(()=>document.querySelector('#actualTx').textContent==='100 台虚拟设备轮播');
   const sim=requests.findLast(x=>x.path==='/api/simulation/start');assert.equal(sim.body.seed,'1234');assert(Number(sim.body.utcMs)>=simBefore && Number(sim.body.utcMs)<=Date.now());assert.match(await page.locator('#simClock').textContent(),/已校时/);
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
   if(process.env.RID_SCREENSHOT_DIR)await page.screenshot({path:path.join(process.env.RID_SCREENSHOT_DIR,mobile?'fleet-mobile.png':'fleet-desktop.png'),fullPage:true});
   await page.locator('#stopSim').click();await page.waitForFunction(()=>document.querySelector('#liveStrip').hidden);
   await page.locator('[data-txmode="single"]').click();await page.locator('#defaults').click();assert.equal(await page.locator('#height').inputValue(),'30');assert.equal(await page.locator('#speed').inputValue(),'0');
   await page.locator('#speed').fill('-1');assert.equal(await page.locator('#txForm').evaluate(f=>f.checkValidity()),false);
   assert.deepEqual(errors,[]);await context.close();console.log(`PASS ${mobile?'mobile':'desktop'}: fields, preview, save, UTC single/fleet start, roster, validation and responsive layout`);
  }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
