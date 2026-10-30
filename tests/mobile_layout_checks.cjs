// Optional real-browser layout check. Requires Playwright and Chromium.
// Every request/command is intercepted: no station, microphone or RF is used.
const {chromium}=require('playwright');
const fs=require('fs'),path=require('path'),assert=require('assert/strict');
const root=path.resolve(__dirname,'..'),output=path.join(root,'artifacts/mobile-layout');fs.mkdirSync(output,{recursive:true});
(async()=>{
 const browser=await chromium.launch({headless:true,...(process.env.CHROME_PATH?{executablePath:process.env.CHROME_PATH}:{})});
 try{
 const page=await browser.newPage({viewport:{width:390,height:844},deviceScaleFactor:1,isMobile:true,hasTouch:true});
 const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.route('http://station.test/**',route=>{
  const name=new URL(route.request().url()).pathname.slice(1)||'index.html';
  if(name.startsWith('api/'))return route.fulfill({status:401,contentType:'application/json',body:'{}'});
  const file=path.join(root,'web',name);return route.fulfill({contentType:name.endsWith('.js')?'application/javascript':name.endsWith('.css')?'text/css':'text/html',body:fs.readFileSync(file)});
 });
 await page.goto('http://station.test/');await page.waitForTimeout(150);
 assert.deepEqual(errors,[],'page setup');
 await page.evaluate(()=>{
  const item=(id,value)=>({id,value:String(value),valid:true,writable:true,type:'real',age:0,revision:0,name:id,group:'Radio',description:'Radio control',range:'',units:'',choices:[],choiceValues:[],display:String(value),status:'Live'});
  const settings=meterSpecs.map(([id],i)=>item(id,[-20,50,1.3,.4,5,13.8,8][i]));
  gainSpecs.forEach(([id])=>settings.push(item(id,.5)));
  settings.push(item('level.PREAMP',0),item('level.NR',.2),item('level.NB',3),item('func.NR',1),item('func.NB',0));
  functionSpecs.forEach(([id])=>settings.push(item(id,0)));
  const next={generation:1,online:true,portOpen:true,busy:false,demo:false,poweredOff:false,pttKnown:true,transmitting:false,canPower:true,canPtt:true,status:'Connected / live CAT',error:'',activeVfo:'A',settingsVfo:'Current',vfoRevision:0,contactAge:0,settings,modes:['USB','LSB','CW','PKTUSB','AM','FM'],vfos:[{frequency:7100000,mode:'USB',width:2400,frequencyValid:true,modeValid:true,frequencyRevision:0,modeRevision:0},{frequency:14074000,mode:'PKTUSB',width:3000,frequencyValid:true,modeValid:true,frequencyRevision:0,modeRevision:0}],connection:{kind:0,endpoint:'COM4',baud:38400,stopBits:1,flow:'None',poll:750},sharing:{wanted:true,running:true,port:4532,clients:2},audio:{devices:[],maps:['','','',''],receiveWanted:false,microphoneWanted:false,receiveText:'Off',microphoneText:'Off',error:'',receiveLevel:100,microphoneLevel:100,pttLimit:0,inputPeak:0,outputPeak:0,micInputPeak:0,micOutputPeak:0},scope:{fresh:false},rfScope:{running:false,frameValid:false,status:'RF capture is off',centerHz:7100000,spanHz:200000,requestedSpanIndex:7,cat2Port:'COM5',cat2Baud:4800,levelDb:-5},activity:[]};
  render(next);window.testCommands=[];command=async(action,fields)=>{testCommands.push({action,fields});return {revision:1};};
 });
 await page.screenshot({path:path.join(output,'operate-390.png'),fullPage:true});
 console.log(await page.evaluate(()=>({height:document.documentElement.scrollHeight,width:document.documentElement.scrollWidth,operateBottom:$('phoneOperator').getBoundingClientRect().bottom,dockTop:$('mobilePttBar').getBoundingClientRect().top})));
 for(const size of [{width:320,height:667},{width:390,height:844},{width:430,height:932}]){
  await page.setViewportSize(size);await page.waitForTimeout(100);
  assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'no horizontal scrolling '+size.width);
  assert.ok(await page.evaluate(()=>$('phoneOperator').getBoundingClientRect().bottom<=$('mobilePttBar').getBoundingClientRect().top),'operating controls clear of dock '+size.width);
 }
 await page.setViewportSize({width:390,height:844});
 await page.evaluate(()=>state.canTune=true);
 await page.locator('#phoneTuner').dblclick();assert.ok(await page.evaluate(()=>testCommands.some(x=>x.action==='tuneAntenna')),'double-tap tuner retains the tuning-cycle action');
 await page.locator('#phoneFrequency').click();assert.equal(await page.locator('#phoneSheet').isVisible(),true);
 await page.locator('#frequency').fill('7.074');await page.locator('#frequencyForm button').click();
 assert.equal(await page.locator('#phoneSheet').isVisible(),false);
 assert.ok(await page.evaluate(()=>testCommands.some(x=>x.action==='frequency'&&x.fields.hz==='7074000')));
 await page.locator('#phoneMode').click();await page.locator('#mode').selectOption('PKTUSB');
 assert.ok(await page.evaluate(()=>testCommands.some(x=>x.action==='mode'&&x.fields.mode==='PKTUSB')));
 // The real PTT control remains reachable with the panel open.
 await page.locator('#ptt').click();assert.ok(await page.evaluate(()=>testCommands.some(x=>x.action==='ptt'&&x.fields.enabled==='1')));
 await page.evaluate(()=>render({...state,dashboardPtt:true,transmitting:true}));await page.locator('#ptt').click();
 assert.ok(await page.evaluate(()=>testCommands.some(x=>x.action==='ptt'&&x.fields.enabled==='0')),'PTT off remains reachable above the panel shade');
 await page.evaluate(()=>render({...state,dashboardPtt:false,transmitting:false}));
 await page.locator('#phoneSheetClose').click();await page.locator('#phoneOtherVfo').click();
 assert.ok(await page.evaluate(()=>testCommands.some(x=>x.action==='vfo'&&x.fields.vfo==='B')));
 await page.locator('#phoneNav [data-page="more"]').click();await page.locator('#more [data-phone-sheet="rf"]').click();
 assert.equal(await page.locator('#rfSpan').isVisible(),true);await page.locator('#phoneSheetClose').click();
 await page.locator('#phoneNav [data-page="dashboard"]').click();
 await page.evaluate(()=>render({...state,transmitting:true}));assert.equal(await page.locator('#phoneMeterStrip .phone-meter').count(),3);
 assert.ok(await page.evaluate(()=>$('phoneOperator').getBoundingClientRect().bottom<=$('mobilePttBar').getBoundingClientRect().top),'TX dock must not cover quick controls');
 await page.screenshot({path:path.join(output,'operate-tx-390.png'),fullPage:true});
 await page.locator('#phoneMode').click();await page.setViewportSize({width:1280,height:900});await page.waitForTimeout(120);
 assert.equal(await page.locator('#phoneSheet').isVisible(),false);assert.equal(await page.locator('#mode').isVisible(),true);
 assert.equal(await page.locator('#frequency').isVisible(),true);assert.equal(await page.locator('#phoneOperator').isVisible(),false);
 assert.equal(await page.locator('#meters').isVisible(),true);await page.screenshot({path:path.join(output,'desktop.png'),fullPage:true});
 await page.setViewportSize({width:390,height:844});await page.waitForTimeout(120);assert.equal(await page.locator('#phoneOperator').isVisible(),true);
 assert.equal(await page.evaluate(()=>{const ids=[...document.querySelectorAll('[id]')].map(n=>n.id);return ids.length===new Set(ids).size;}),true,'relocation must not duplicate control IDs');
 assert.deepEqual(errors,[],'runtime errors');console.log('Mobile layout/interactions passed at 320/390/430 px; sheets, tuning, mode, PTT access, VFO, RF controls and desktop restoration.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
