const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const path=require('node:path');
const {PreferenceSaver,TuningQueue,RfHistory}=require('../web/control-core.js');
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function checkPlaybackMicWorkaround(){
  const nodes=new Map(),commands=[],gates=[],sessions=[],saved=new Map(),notices=[];
  const document={hidden:false,getElementById(id){if(!nodes.has(id))nodes.set(id,{value:'',checked:false,textContent:'',classList:{toggle(){}}});return nodes.get(id);}};
  const get=id=>document.getElementById(id);
  let sessionType='playback',track,callbacks,requested,failCapture=false,rejectPlayback=false,endOnPlayback=false;
  const session={state:'active'};
  Object.defineProperty(session,'type',{get:()=>sessionType,set(value){
    sessions.push(value);
    if(value==='playback'&&rejectPlayback&&track?.readyState==='live')throw new Error('Capture requires play-and-record');
    sessionType=value;
    if(value==='playback'&&endOnPlayback&&track?.readyState==='live'){
      track.readyState='ended';callbacks.ended?.();
    }
  }});
  const navigator={audioSession:session,mediaDevices:{async getUserMedia(options){
    assert.equal(sessionType,'play-and-record','permission/device access needs a capture-compatible session');
    requested=options;if(failCapture)throw new Error('Microphone permission denied');
    callbacks={};track={readyState:'live',muted:false,stopped:false,stop(){this.stopped=true;this.readyState='ended';},addEventListener(name,handler){callbacks[name]=handler;}};
    return {getTracks:()=>[track],getAudioTracks:()=>[track]};
  }}};
  const context=vm.createContext({console,document,navigator,PreferenceSaver,TuningQueue,RfHistory,performance,URLSearchParams,ArrayBuffer,DataView,Uint8Array,Uint8ClampedArray,Float32Array,Int16Array,setTimeout,clearTimeout,clearInterval,requestAnimationFrame:()=>{},window:{isSecureContext:true,AudioWorkletNode:function(){}},localStorage:{setItem:(key,value)=>saved.set(key,value)}});
  const source=fs.readFileSync(path.join(__dirname,'../web/app.js'),'utf8');vm.runInContext(source.replace(/\bsetup\(\);\s*$/,''),context);
  assert.equal(vm.runInContext('browserPlaybackGain(100,0)',context),1);
  assert.equal(vm.runInContext('browserPlaybackGain(0,24)',context),0);
  assert.ok(Math.abs(vm.runInContext('browserPlaybackGain(100,6)',context)-1.995262)<.000001);
  assert.ok(Math.abs(vm.runInContext('browserPlaybackGain(50,12)',context)-1.990536)<.000001);
  assert.ok(vm.runInContext('browserPlaybackGain(100,24)',context)>15.8);
  assert.equal(vm.runInContext('browserPlaybackGain(-10,24)',context),0);
  assert.equal(vm.runInContext('browserPlaybackGain(300,99)',context),vm.runInContext('browserPlaybackGain(100,24)',context));
  assert.equal(vm.runInContext('browserPlaybackGain(NaN,NaN)',context),vm.runInContext('browserPlaybackGain(100,6)',context));
  const curve=vm.runInContext('browserPlaybackCurve()',context);
  assert.equal(curve.length,4097);assert.equal(curve[2048],0);assert.equal(curve[2560],.25);
  assert.ok(curve[0]>-1&&curve[4096]<1);
  for(let i=1;i<curve.length;i++)assert.ok(curve[i]>=curve[i-1]);
  get('browserVolume').value='100';get('browserBoostDb').value='12';
  vm.runInContext('listeningGain={gain:{value:0}};applyBrowserPlaybackLevel();',context);
  assert.ok(vm.runInContext('listeningGain.gain.value',context)>3.98);
  get('browserVolume').value='0';vm.runInContext('applyBrowserPlaybackLevel()',context);
  assert.equal(vm.runInContext('listeningGain.gain.value',context),0);
  context.record=(action,fields)=>{commands.push({action,fields});return Promise.resolve({});};
  context.captureGate=message=>gates.push(message);
  context.recordNotice=message=>notices.push(message);
  vm.runInContext("state={online:true,demo:false,browserOwner:2,transmitting:false,dashboardPtt:false,audio:{maps:['','','','radio:output']}};client=1;socket={readyState:1};command=record;renderAudio=()=>{};updateDiagnostics=()=>{};browserDevices=async()=>{};notify=recordNotice;ensureAudio=async()=>{};ensureReceive=async()=>{};audioContext={resume:async()=>{},createMediaStreamSource:()=>({connect(){},disconnect(){}})};audioNode={port:{postMessage:captureGate}};listening=true;",context);
  get('stableAudio').checked=true;
  vm.runInContext('audioSessionType()',context);assert.equal(sessionType,'play-and-record');
  get('playbackMicWorkaround').checked=true;
  vm.runInContext('audioSessionType()',context);assert.equal(sessionType,'playback');
  sessions.length=0;
  await vm.runInContext('armMicrophone()',context);
  assert.deepEqual(sessions,['play-and-record','playback']);
  assert.equal(requested.audio.echoCancellation,false);assert.equal(requested.audio.noiseSuppression,false);assert.equal(requested.audio.autoGainControl,false);
  assert.equal(vm.runInContext('microphone&&playbackMicActive',context),true);assert.equal(track.readyState,'live');
  assert.equal(commands.some(item=>item.action==='ptt'),false,'arming must not key the radio');
  assert.equal(gates.some(item=>item.kind==='gate'&&item.enabled),false,'receive capture must not open the TX gate');
  await vm.runInContext('listen()',context);assert.equal(sessionType,'playback');
  await vm.runInContext('listen()',context);assert.equal(sessionType,'playback');
  assert.equal(vm.runInContext('microphone',context),true,'listening changes must preserve the active mic');
  vm.runInContext('state.browserOwner=client;',context);callbacks.mute();
  assert.equal(vm.runInContext('microphone||playbackMicActive',context),false);assert.equal(track.stopped,true);
  assert.equal(get('playbackMicWorkaround').checked,false);assert.equal(saved.get('playbackMicWorkaround'),'0');
  assert.ok(commands.some(item=>item.action==='ptt'&&item.fields.enabled==='0'),'an interrupted owned microphone must release PTT');
  assert.match(get('browserStatus').textContent,/workaround switched off/);

  vm.runInContext('state.browserOwner=2;',context);get('playbackMicWorkaround').checked=true;rejectPlayback=true;
  await vm.runInContext('armMicrophone()',context);
  assert.equal(vm.runInContext('microphone',context),true);assert.equal(vm.runInContext('playbackMicActive',context),false);
  assert.equal(sessionType,'play-and-record');assert.equal(get('playbackMicWorkaround').checked,false);
  assert.ok(notices.some(message=>message.includes('Using standard microphone audio')));
  vm.runInContext('stopMicrophone()',context);rejectPlayback=false;

  get('playbackMicWorkaround').checked=true;endOnPlayback=true;
  await assert.rejects(vm.runInContext('armMicrophone()',context),/Microphone did not remain active/);
  assert.equal(vm.runInContext('microphone||microphoneBusy||playbackMicActive',context),false);assert.equal(track.stopped,true);
  assert.equal(get('playbackMicWorkaround').checked,false);endOnPlayback=false;

  get('playbackMicWorkaround').checked=true;failCapture=true;
  await assert.rejects(vm.runInContext('armMicrophone()',context),/permission denied/);
  assert.equal(vm.runInContext('microphone||microphoneBusy||micRegistered',context),false);
  assert.equal(sessionType,'playback');assert.equal(get('playbackMicWorkaround').checked,true,'permission denial must not be misreported as a failed workaround');
  delete navigator.audioSession;vm.runInContext('audioSessionType(true)',context);
  assert.equal(vm.runInContext('playbackMicActive',context),false);
}
async function checkBrowserConnections(){
  const nodes=new Map(),timers=new Map(),windows=new Map(),documents=new Map(),connections=[],rendered=[],notices=[];
  let now=0,nextTimer=0,fetchReply={status:200,ok:true};
  const document={hidden:false,getElementById(id){if(!nodes.has(id))nodes.set(id,{value:'',textContent:'',hidden:false,classList:{toggle(){}},disabled:false});return nodes.get(id);},addEventListener:(name,fn)=>documents.set(name,fn)};
  const addTimer=(fn,ms,interval=false)=>{const id=++nextTimer;timers.set(id,{fn,ms,interval});return id;};
  const clearTimer=id=>timers.delete(id);
  const runTimeout=async ms=>{const entry=[...timers].find(([,timer])=>!timer.interval&&timer.ms===ms);assert.ok(entry,'expected timer '+ms);timers.delete(entry[0]);await entry[1].fn();};
  const pulse=()=>{const timer=[...timers.values()].find(timer=>timer.interval);assert.ok(timer);timer.fn();};
  class FakeSocket{
    static OPEN=1;
    constructor(){if(FakeSocket.fail)throw new Error('Socket could not be opened');this.readyState=0;this.bufferedAmount=0;this.sent=[];connections.push(this);}
    send(value){this.sent.push(value);}
    close(code=1000,reason=''){this.readyState=3;this.onclose?.({code,reason});}
    open(){this.readyState=1;this.onopen();}
    message(value){this.onmessage({data:JSON.stringify(value)});}
  }
  const context=vm.createContext({console,document,navigator:{},window:{addEventListener:(name,fn)=>windows.set(name,fn)},location:{host:'station:8888'},WebSocket:FakeSocket,AbortController,fetch:()=>Promise.resolve(fetchReply),performance:{now:()=>now},PreferenceSaver,TuningQueue,RfHistory,URLSearchParams,ArrayBuffer,DataView,Uint8Array,Uint8ClampedArray,Float32Array,Int16Array,setTimeout:(fn,ms)=>addTimer(fn,ms),clearTimeout:clearTimer,setInterval:(fn,ms)=>addTimer(fn,ms,true),clearInterval:clearTimer,requestAnimationFrame:()=>{}});
  const source=fs.readFileSync(path.join(__dirname,'../web/app.js'),'utf8');vm.runInContext(source.replace(/\bsetup\(\);\s*$/,''),context);
  context.rendered=next=>rendered.push(next);context.notice=message=>notices.push(message);
  vm.runInContext("state={generation:1};render=next=>{state=next;rendered(next);};renderAudio=()=>{};subscribeScopes=()=>{};scheduleScopes=()=>{};stopClientAudio=()=>{microphone=false;listening=false;};notify=notice;",context);
  await vm.runInContext('connectWeb()',context);const first=connections.at(-1);first.open();
  first.message({type:'hello',client:1});first.message({type:'state',generation:1});
  assert.equal([...timers.values()].some(timer=>timer.ms===8000),false,'initial state clears the handshake timer');
  assert.ok(first.sent.includes('action=heartbeat'));
  now=20000;pulse();assert.equal(first.readyState,1,'one long JavaScript pause must allow a fresh heartbeat reply');
  first.message({type:'heartbeat'});
  vm.runInContext("microphone=listening=true;var lostRequests=0;pending.set('test',{timer:0,reject:()=>lostRequests++});",context);
  for(let i=0;i<8;i++){now+=2000;pulse();}
  assert.equal(first.readyState,3);assert.equal(vm.runInContext('socket',context),null);
  assert.equal(vm.runInContext('microphone||listening',context),false);assert.equal(vm.runInContext('lostRequests',context),1);
  assert.match(vm.runInContext('lastDisconnect',context),/heartbeat reply timed out/);
  assert.equal(document.getElementById('ptt').disabled,true);
  await runTimeout(2000);const second=connections.at(-1);assert.notEqual(second,first);
  const renderCount=rendered.length;second.open();second.message({type:'delta',generation:2});
  assert.equal(rendered.length,renderCount,'reconnect must not merge deltas into the old connection state');
  first.message({type:'state',generation:999});first.onclose({code:1006,reason:'stale'});
  assert.equal(vm.runInContext('socket',context),second,'old connection events must not retire its replacement');
  await runTimeout(8000);assert.equal(second.readyState,3,'an open socket without an initial state must time out');
  fetchReply={status:401,ok:false};await runTimeout(2000);
  assert.equal(document.getElementById('login').hidden,false);assert.equal(document.getElementById('app').hidden,true);
  assert.equal(document.getElementById('mobilePttBar').hidden,true);

  let resolveProbe;context.fetch=()=>new Promise(resolve=>resolveProbe=resolve);
  vm.runInContext('reconnect()',context);
  const probe=runTimeout(2000);await Promise.resolve();
  await vm.runInContext('connectWeb()',context);const third=connections.at(-1);third.open();
  document.getElementById('login').hidden=true;resolveProbe({status:401,ok:false});await probe;
  assert.equal(document.getElementById('login').hidden,true,'a stale reconnect probe must not replace a new connection with the login page');
  third.message({type:'state',generation:3});third.message({type:'heartbeat'});
  vm.runInContext('microphone=listening=true;',context);third.message({type:'controlIdle'});
  assert.equal(vm.runInContext('microphone',context),false);assert.equal(vm.runInContext('listening',context),true,'control pause must not stop receive playback');
  vm.runInContext('installPageLifecycle()',context);now+=20000;pulse();assert.equal(third.readyState,1);
  now+=20000;pulse();assert.equal(third.readyState,3,'successive delayed ticks without a fresh reply must not extend the grace period forever');
  windows.get('online')();assert.ok([...timers.values()].some(timer=>timer.ms===250));
  FakeSocket.fail=true;await vm.runInContext('connectWeb()',context);assert.match(vm.runInContext('lastDisconnect',context),/Cannot open station WebSocket/);
  windows.get('pagehide')();assert.equal(vm.runInContext('closing',context),true);
  assert.equal(timers.size,0,'page close must cancel reconnect, handshake and heartbeat timers');
}
async function main(){
  await checkBrowserConnections();
  await checkPlaybackMicWorkaround();
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
  console.log('Browser checks passed: preference ordering/failure, tuning/cancellation, RF grids/freshness, mic ownership, page restoration, incremental waterfall pixels, playback-session ordering/capture/rejection/interruption recovery, playback boost/mute/peak guard, connection watchdog/reconnect races/stale sockets/idle mic release.');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
