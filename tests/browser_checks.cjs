const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const path=require('node:path');
const {PreferenceSaver,TuningQueue,RfHistory}=require('../web/control-core.js');
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function main(){
  let draft={endpoint:'COM4'},requests=[],settled=[];
  const saver=new PreferenceSaver(()=>({...draft}),fields=>new Promise((resolve,reject)=>requests.push({fields,resolve,reject})),key=>settled.push(key),()=>{},10000);
  saver.edit('endpoint');const first=saver.flush();
  draft.endpoint='COM6';saver.edit('endpoint');requests[0].resolve({});await first;
  assert.deepEqual(settled,[]);assert.equal(saver.versions.has('endpoint'),true);
  clearTimeout(saver.timer);const second=saver.flush();assert.equal(requests[1].fields.endpoint,'COM6');requests[1].resolve({});await second;
  assert.deepEqual(settled,['endpoint']);assert.equal(saver.versions.size,0);
  saver.edit('endpoint');const failed=saver.flush();requests[2].reject(new Error('Offline'));await failed;
  assert.equal(saver.versions.has('endpoint'),true);clearTimeout(saver.timer);
  const invalidSaver=new PreferenceSaver(()=>{throw Error('Invalid draft');},()=>{throw Error('Must not send invalid draft');},()=>{},()=>{},10000);
  invalidSaver.edit('rfWaterfallLevel');await invalidSaver.flush();assert.equal(invalidSaver.versions.has('rfWaterfallLevel'),true);assert.equal(invalidSaver.busy,false);clearTimeout(invalidSaver.timer);

  const moves=[],dial=new TuningQueue(async(vfo,hz)=>moves.push([vfo,hz]),20);
  for(let i=0;i<30;i++)dial.add('A',100);await wait(35);
  assert.equal(moves.reduce((sum,[,hz])=>sum+hz,0),3000);
  const initial=moves.length;for(let i=0;i<10;i++){dial.add('B',100);await wait(7);}assert.ok(moves.length>initial,'continuous movement must flush before input ends');await dial.flush();dial.clear();
  assert.equal(moves.filter(([vfo])=>vfo==='B').reduce((sum,[,hz])=>sum+hz,0),1000);
  dial.add('A',100);dial.clear();await wait(30);assert.equal(moves.reduce((sum,[vfo,hz])=>sum+(vfo==='A'?hz:0),0),3000);
  dial.add('A',2100000);await dial.flush();dial.clear();assert.ok(moves.every(([,hz])=>Math.abs(hz)<=1000000));

  const history=new RfHistory(3);history.push(7100000,100000,1,1,new Uint8Array([1]));history.push(14200000,500000,1,2,new Uint8Array([2]));
  assert.equal(history.rows.length,1);assert.equal(history.rows[0][0],2);
  assert.equal(history.push(14200000,500000,1,2,new Uint8Array([3])),false);
  history.push(14200000,500000,2,3,new Uint8Array([4]));assert.equal(history.rows.length,1);
  history.received=performance.now()-1100;assert.equal(history.fresh(),false);

  const nodes=new Map(),commands=[];
  const document={hidden:false,activeElement:null,getElementById(id){if(!nodes.has(id))nodes.set(id,{id,value:'',textContent:'',hidden:false,classList:{toggle(){}},setAttribute(){},disabled:false});return nodes.get(id);}};
  const events=new Map();
  const context=vm.createContext({console,PreferenceSaver,TuningQueue,RfHistory,performance,URLSearchParams,ArrayBuffer,DataView,Uint8Array,Uint8ClampedArray,Float32Array,Int16Array,document,window:{addEventListener:(name,callback)=>events.set(name,callback)},navigator:{audioSession:{type:'auto',state:'active'}},setTimeout,clearTimeout,clearInterval,requestAnimationFrame:()=>{}});
  const source=fs.readFileSync(path.join(__dirname,'../web/app.js'),'utf8');vm.runInContext(source.replace(/\bsetup\(\);\s*$/,''),context);
  context.record=(action,fields)=>{commands.push({action,fields});return Promise.resolve({});};
  vm.runInContext("state={browserOwner:2,audio:{}};client=1;microphone=true;micRegistered=true;command=record;renderAudio=()=>{};",context);
  await vm.runInContext('armMicrophone()',context);
  assert.deepEqual(commands.map(x=>x.action),['browserMic']);assert.equal(commands[0].fields.enabled,'0');assert.equal(commands.some(x=>x.action==='ptt'),false);
  vm.runInContext("state.online=true;state.busy=false;state.rfScope={frameValid:false};rfBins=new Uint8Array(850);",context);
  assert.throws(()=>vm.runInContext("tuneRfFromCanvas({},'rfSpectrumCanvas')",context),/fresh RF scope/);
  vm.runInContext('var reconnects=0;connectWeb=()=>{reconnects++;};socket={close(){}};installPageLifecycle();',context);
  events.get('pagehide')();assert.equal(vm.runInContext('closing',context),true);
  events.get('pageshow')({persisted:true});assert.equal(vm.runInContext('closing',context),false);assert.equal(vm.runInContext('reconnects',context),1);
  assert.equal(vm.runInContext('microphone',context),false);
  document.createElement=()=>{const canvas={width:0,height:0,data:null};canvas.getContext=()=>({createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)}),putImageData(image,x,y){if(!canvas.data)canvas.data=new Uint8ClampedArray(canvas.width*canvas.height*4);canvas.data.set(image.data,y*canvas.width*4);},drawImage(source,sx,sy,w,h,dx,dy){const copy=source.data.slice(sy*w*4,(sy+h)*w*4);canvas.data.set(copy,dy*w*4);}});return canvas;};
  vm.runInContext('var pixelCalls=0,waterRows=[{value:10}];var painter=(row,x,width,data,offset)=>{pixelCalls++;data[offset]=row?.value||0;data[offset+3]=255;};var image=waterfallImage("test",850,200,waterRows,"grid",painter);',context);
  assert.equal(vm.runInContext('pixelCalls',context),170000);
  vm.runInContext('pixelCalls=0;waterfallImage("test",850,200,waterRows,"grid",painter);',context);assert.equal(vm.runInContext('pixelCalls',context),0);
  vm.runInContext('waterRows.unshift({value:20});waterfallImage("test",850,200,waterRows,"grid",painter);',context);
  assert.equal(vm.runInContext('pixelCalls',context),850);assert.equal(vm.runInContext('image.data[0]',context),20);assert.equal(vm.runInContext('image.data[850*4]',context),10);
  console.log('Browser checks passed: preference ordering/failure, tuning/cancellation, RF grids/freshness, mic ownership, page restoration, incremental waterfall pixels.');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
