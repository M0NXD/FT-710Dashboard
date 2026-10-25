'use strict';
const $ = id => document.getElementById(id), dirty = new Set(), pending = new Map(), edits = new Map();
let state, socket, client = 0, sequence = 0, closing = false, reconnectTimer, reconnectDelay = 2000, heartbeat, noticeTimer, preferenceSaveTimer, selectedSetting, stickyOffsetsObserver, stickyNoticeObserver;
let audioContext, audioSetup, audioNode, listeningGain, micStream, micSource, listening = false, microphone = false, expectedAudio;
let audioEpoch = 0, microphoneBusy = false, listeningBusy = false, rfCaptureAction = '', rfCaptureTimer, rfCaptureBaselineStatus = '';
let preferenceSaver, tuningQueue, activePage='dashboard', renderScheduled=false, renderCost=0, lastCommandMs=0, micRegistered=false;
let playbackMicActive=false;
let micMeter={peak:0,input:0,present:false,limited:false,seen:null};
let microphoneReturn=null,stateRevision=0,microphoneReturnProbe=0;
let connectionTimer,connectionRevision=0,lastDisconnect='None';
let tunerClickTimer=0, tunerClickGeneration=0;
const rfHistory=new RfHistory(), waterfallCaches=new Map();
const browserMicAvailable=()=>!!(window.isSecureContext&&navigator.mediaDevices?.getUserMedia&&window.AudioWorkletNode);
const meterSpecs = [
  ['level.STRENGTH','SIGNAL',v=>v>=0?'S9'+(v>0?' +'+Math.round(v):''):'S'+Math.max(0,Math.min(9,Math.round((v+54)/6))),v=>(v+54)/114],
  ['level.RFPOWER_METER_WATTS','RF OUTPUT',v=>v.toFixed(1)+' W',v=>v/100],
  ['level.SWR','SWR',v=>v.toFixed(1)+' : 1',v=>(v-1)/4],
  ['level.ALC','ALC',v=>(v*100).toFixed(1)+' %',v=>v],
  ['level.COMP_METER','COMPRESSION',v=>v.toFixed(1)+' dB',v=>v/20],
  ['level.VD_METER','SUPPLY',v=>v.toFixed(1)+' V',v=>v/16],
  ['level.ID_METER','DRAIN CURRENT',v=>v.toFixed(1)+' A',v=>v/25]
];
const gainSpecs = [['level.AF','AF gain (%)',100],['level.RF','RF gain (%)',100],['level.RFPOWER','RF power (W)',100],['level.MICGAIN','Mic gain (%)',100],['level.SQL','Squelch (%)',100],['level.IF','IF shift (Hz)',1]];
const cycleSpecs = [['preamp','Preamp: IPO'],['dnr','DNR: Off'],['nb','NB: Off']];
const functionSpecs = [['func.ANF','Auto notch'],['func.APF','APF'],['func.MON','Monitor'],['func.COMP','Processor'],['func.VOX','VOX'],['func.LOCK','Dial lock'],['func.TUNER','Tuner']];
const meterElements = [], gainElements = [], cycleElements = [], functionElements = [], settingRows = new Map();
function element(tag, text, className) { const node = document.createElement(tag); if(text !== undefined) node.textContent=text; if(className) node.className=className; return node; }
function setText(node, text) { const value=String(text ?? ''); if(node.textContent!==value) node.textContent=value; }
function value(id, next) { const node=$(id); if(!dirty.has(id) && document.activeElement!==node && !edits.has(id)) node.value=String(next ?? ''); }
function notify(message, error=true) { clearTimeout(noticeTimer); $('notice').textContent=message; $('notice').classList.toggle('error',error); if(message) noticeTimer=setTimeout(()=>$('notice').textContent='',error?12000:4500); }
function protect(fn) { return (...args)=>{try{return Promise.resolve(fn(...args)).catch(e=>notify(e.message||String(e)));}catch(e){notify(e.message||String(e));}}; }
function updateRfCaptureButtons(rf=state?.rfScope||{}) {
  const status=rf.status||'RF capture is off';
  if(rfCaptureAction==='start'&&rf.running)rfCaptureAction='';
  else if(rfCaptureAction==='start'&&!rf.running&&status!==rfCaptureBaselineStatus&&!/opening|starting|waiting/i.test(status))rfCaptureAction='';
  else if(rfCaptureAction==='stop'&&!rf.running)rfCaptureAction='';
  if(!rfCaptureAction)clearTimeout(rfCaptureTimer);
  for(const [id,short] of [['rfCapture',false],['mobileRfCapture',true]]) {
    const button=$(id);if(!button)continue;
    const label=rfCaptureAction==='start'?(short?'Starting…':'Starting RF capture…'):rfCaptureAction==='stop'?(short?'Stopping…':'Stopping RF capture…'):rf.running?(short?'Stop':'Stop RF capture'):(short?'Start':'Start RF capture');
    setText(button,label);button.classList.toggle('selected',!!rf.running);button.disabled=!!rfCaptureAction;
    button.setAttribute('aria-label',rfCaptureAction?label:rf.running?'Stop RF capture':'Start RF capture');
  }
}
async function toggleRfCapture() {
  if(rfCaptureAction||!state)return;
  const action=state.rfScope?.running?'rfStop':'rfStart';rfCaptureAction=action==='rfStart'?'start':'stop';rfCaptureBaselineStatus=state.rfScope?.status||'';
  updateRfCaptureButtons();
  rfCaptureTimer=setTimeout(()=>{
    if(!rfCaptureAction)return;
    const pendingAction=rfCaptureAction;rfCaptureAction='';updateRfCaptureButtons();
    const status=state?.rfScope?.status||'No capture status received';
    notify(pendingAction==='start'?'RF capture did not start: '+status:'RF capture did not stop: '+status);
  },8000);
  try { await command(action); }
  catch(error) { rfCaptureAction='';clearTimeout(rfCaptureTimer);updateRfCaptureButtons();throw error; }
}
function command(action, fields={}, editKey) {
  if(!socket || socket.readyState!==WebSocket.OPEN || !state) return Promise.reject(new Error('Wait for the station connection'));
  const seq=String(++sequence), payload=new URLSearchParams({action,seq,generation:String(state.generation),...fields});
  return new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{ pending.delete(seq); if(editKey&&edits.get(editKey)?.seq===seq) edits.delete(editKey); reject(new Error('No command acknowledgement; check the station connection')); },10000);
    pending.set(seq,{resolve,reject,timer,editKey,sent:performance.now()}); if(editKey) edits.set(editKey,{seq,revision:null,until:Date.now()+12000});
    try{socket.send(payload.toString());}catch(error){clearTimeout(timer);pending.delete(seq);if(editKey&&edits.get(editKey)?.seq===seq)edits.delete(editKey);reject(error);}
  });
}
const preferenceIds=['connectionKind','endpoint','baud','stopBits','flow','poll','rfSpan','rfCat2Port','rfCat2Baud','rfWaterfallLevel'];
function readStationPreferences() {
    const fields={kind:$('connectionKind').value,endpoint:$('endpoint').value};
    for(const key of ['baud','stopBits','flow','poll'])fields[key]=$(key).value;
    fields.rfSpan=$('rfSpan').value;fields.rfCat2Port=$('rfCat2Port').value.trim();fields.rfCat2Baud=$('rfCat2Baud').value;
    const levelText=$('rfWaterfallLevel').value.trim(),level=Number(levelText);
    if(preferenceSaver?.versions.has('rfWaterfallLevel')&&(levelText===''||!Number.isFinite(level)||level < -30||level > 30||!Number.isInteger(level*2)))throw new Error('Waterfall level must be -30 to +30 dB in 0.5 dB steps');
    if(levelText!==''&&Number.isFinite(level)&&level>=-30&&level<=30&&Number.isInteger(level*2))fields.rfWaterfallLevel=level.toFixed(1);
    return fields;
}
function saveStationPreferencesSoon(event) {
  const key=event?.target?.id;if(!preferenceIds.includes(key))return;
  dirty.add(key);preferenceSaver.edit(key);
}
function subscribeScopes() {
  if(socket?.readyState===1)command('subscribe',{audio:activePage==='scope'&&!document.hidden?'1':'0',rf:['dashboard','rfScope'].includes(activePage)&&!document.hidden?'1':'0'}).catch(()=>{});
}
function scheduleScopes() {
  if(renderScheduled||document.hidden)return;renderScheduled=true;
  requestAnimationFrame(()=>{renderScheduled=false;const started=performance.now();drawScope();drawRfScope();renderCost=performance.now()-started;updateDiagnostics();});
}
function updateDiagnostics() {
  if(!$('diagnosticText')||!state)return;
  const rf=state.rfScope||{};
  setText($('diagnosticText'),`CAT: ${state.connection.endpoint} · last reply ${state.contactAge>=0?(state.contactAge/1000).toFixed(1)+' s ago':'unavailable'}\nRF: ${rf.running?(rf.frameValid?'live':'waiting / stale'):'stopped'} · age ${rf.age>=0?rf.age+' ms':'unavailable'}\nBrowser: ${socket?.readyState===1?'connected':'disconnected'} · command acknowledgement ${lastCommandMs.toFixed(0)} ms\nLast disconnect: ${lastDisconnect}\nAudio: ${audioContext?.state||'off'} · session ${navigator.audioSession?.type||'system default'} / ${navigator.audioSession?.state||'unknown'}\nDraw: ${renderCost.toFixed(1)} ms · ${state.audio.refreshing?'refreshing devices':state.audio.receiveText}\nMicrophone: ${microphone?'armed':'disarmed'} · ${state.releasePending?'PTT release retry pending':'PTT release clear'}`);
}
function setting(id) { return state?.settings?.find(item=>item.id===id); }
function discreteLevel(item, maximum, normalized) {
  const value=Number(item?.value);if(!item?.valid||!Number.isFinite(value))return -1;
  return Math.max(0,Math.min(maximum,Math.round(value*(normalized?maximum:1))));
}
function cycleControlState(name) {
  if(name==='preamp'){
    const item=setting('level.PREAMP');if(!item?.valid)return {label:'Preamp: --',enabled:false,selected:false};
    const labels={'0':'IPO','10':'AMP1','20':'AMP2'};return {label:'Preamp: '+(labels[item.value]||item.value),enabled:!!item.writable,selected:item.value!=='0'};
  }
  const dnr=name==='dnr',functionItem=setting(dnr?'func.NR':'func.NB'),level=setting(dnr?'level.NR':'level.NB'),maximum=dnr?15:10;
  if(!functionItem?.valid)return {label:(dnr?'DNR':'NB')+': --',enabled:false,selected:false};
  const enabled=functionItem.value!=='0',amount=discreteLevel(level,maximum,dnr),label=enabled?(amount<0?'On':String(amount)):'Off';
  return {label:(dnr?'DNR':'NB')+': '+label,enabled:!!functionItem.writable&&!!level?.valid&&!!level.writable,selected:enabled};
}
async function cycleControl(name) {
  if(name==='preamp'){
    const item=setting('level.PREAMP');if(!item?.valid||!item.writable)throw new Error('Preamp readback is not available');
    const next=item.value==='0'?'10':item.value==='10'?'20':'0';return command('set',{setting:item.id,value:next});
  }
  const dnr=name==='dnr',functionItem=setting(dnr?'func.NR':'func.NB'),level=setting(dnr?'level.NR':'level.NB'),maximum=dnr?15:10;
  const amount=discreteLevel(level,maximum,dnr);
  if(!functionItem?.valid||!functionItem.writable||!level?.valid||!level.writable||amount<0)throw new Error((dnr?'DNR':'NB')+' readback is not available');
  if(functionItem.value==='0'){
    await command('set',{setting:level.id,value:dnr?String(1/15):'1'});
    return command('set',{setting:functionItem.id,value:'1'});
  }
  if(amount>=maximum)return command('set',{setting:functionItem.id,value:'0'});
  return command('set',{setting:level.id,value:dnr?String((amount+1)/maximum):String(amount+1)});
}
function editAcknowledged(key, revision) { const edit=edits.get(key); if(edit && ((edit.revision!==null && revision>=edit.revision) || Date.now()>edit.until)) { edits.delete(key); dirty.delete(key); } }
function optionList(node, entries, selected) {
  const signature=JSON.stringify(entries); if(node.dataset.options!==signature) { const previous=node.value;node.replaceChildren(...entries.map(([label,value])=>{const option=element('option',label);option.value=value;return option;}));node.dataset.options=signature;if(document.activeElement===node||dirty.has(node.id))node.value=previous; }
  if(document.activeElement!==node && !dirty.has(node.id)) node.value=String(selected ?? '');
}
function showPage(name) { activePage=name;if(name==='settings'&&state)renderSettings();document.querySelectorAll('.page').forEach(node=>node.hidden=node.id!==name);document.querySelectorAll('[data-page]').forEach(node=>node.classList.toggle('selected',node.dataset.page===name));subscribeScopes();scheduleScopes();const tab=document.querySelector(`[data-page="${name}"]`);if(tab&&window.matchMedia('(max-width: 650px)').matches)tab.scrollIntoView({behavior:'smooth',block:'nearest',inline:'center'});window.scrollTo(0,0); }
function updateStickyNavigation(){const header=document.querySelector('.topbar'),notice=$('notice');if(!header||!notice)return;const headerHeight=Math.ceil(header.getBoundingClientRect().height),noticeHeight=notice.getBoundingClientRect().height;document.documentElement.style.setProperty('--topbar-height',headerHeight+'px');document.documentElement.style.setProperty('--section-tabs-top',headerHeight+noticeHeight+'px');}
function frequency(hz) { return Math.round(hz).toString().padStart(8,'0').replace(/(\d+)(\d{3})(\d{3})$/,'$1.$2.$3'); }
function parseFrequencyInput(input){let text=String(input).trim().toLowerCase().replace(/\s+/g,'');let multiplier=0;for(const [unit,scale]of [['mhz',1e6],['khz',1e3],['hz',1]])if(text.endsWith(unit)){text=text.slice(0,-unit.length);multiplier=scale;break;}const separators=(text.match(/[.,]/g)||[]).length;if(separators>1)text=text.replace(/[.,]/g,'');else text=text.replace(',','.');const match=text.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))$/);if(!match)throw new Error('Enter a frequency such as 7.074 MHz, 7074 kHz, or 7074000 Hz');const value=Number(match[1]);if(!Number.isFinite(value))throw new Error('Enter a valid radio frequency');if(!multiplier)multiplier=value<=60?1e6:value<=60000?1e3:1;const hz=Math.round(value*multiplier);if(hz<30000||hz>60000000)throw new Error('Frequency must be between 0.030 and 60 MHz');return hz;}
function band(hz) { for(const [lo,hi,name] of [[1.8e6,2e6,'160 m'],[3.5e6,4e6,'80 m'],[5.25e6,5.45e6,'60 m'],[7e6,7.3e6,'40 m'],[10.1e6,10.15e6,'30 m'],[14e6,14.35e6,'20 m'],[18.068e6,18.168e6,'17 m'],[21e6,21.45e6,'15 m'],[24.89e6,24.99e6,'12 m'],[28e6,29.7e6,'10 m'],[50e6,54e6,'6 m']]) if(hz>=lo&&hz<=hi)return name;return 'General coverage'; }
function cancelTunerClick(){clearTimeout(tunerClickTimer);tunerClickTimer=0;}
function toggleTuner(){
  const item=setting('func.TUNER');
  if(!state?.online||state.busy||!item?.valid||!item.writable)return;
  return command('set',{setting:item.id,value:item.value==='0'?'1':'0'});
}
async function tunerClick(event){
  event.preventDefault();
  if(event.detail===0){cancelTunerClick();return toggleTuner();}
  if(tunerClickTimer&&tunerClickGeneration===state?.generation){
    cancelTunerClick();
    if(!state?.online||state.busy||!state.canTune)throw new Error('Antenna tuning is unavailable');
    if(!state.pttKnown||state.transmitting||state.dashboardPtt||state.pttPending||state.releasePending)
      throw new Error('Wait for RX and release PTT before antenna tuning');
    await command('tuneAntenna');notify('Antenna tuning cycle requested',false);return;
  }
  cancelTunerClick();tunerClickGeneration=state?.generation;
  const generation=tunerClickGeneration;
  tunerClickTimer=setTimeout(protect(()=>{
    tunerClickTimer=0;
    if(!closing&&!document.hidden&&state?.generation===generation)return toggleTuner();
  }),500);
}
function initializeCards() {
  for(const spec of meterSpecs) {
    const card=element('article',undefined,'panel meter-card'), title=element('span',spec[1],'meter-title'), output=element('output','--','meter-value'), bar=element('div',undefined,'meter-bar'), fill=element('i'), note=element('small','No live reading','meter-note');
    bar.append(fill);card.append(title,output,bar,note);$('meters').append(card);meterElements.push({output,fill,note});
  }
  gainSpecs.forEach((spec,index)=>{
    const form=element('form',undefined,'gain-form'), label=element('div',undefined,'gain-label'), title=element('b',spec[1]), readback=element('small','Radio: --'), input=element('input'), button=element('button','Set');
    input.id='gain'+index;input.type='number';input.step='any';input.setAttribute('aria-label',spec[1]);input.addEventListener('input',()=>dirty.add(input.id));label.append(title,readback);form.append(label,input,button);$('quickGains').append(form);gainElements.push({input,button,readback});
    form.addEventListener('submit',protect(async event=>{event.preventDefault();await command('set',{setting:spec[0],value:String(Number(input.value)/spec[2])},input.id);}));
  });
  cycleSpecs.forEach(([name,initial])=>{const button=element('button',initial);button.addEventListener('click',protect(()=>cycleControl(name)));$('functions').append(button);cycleElements.push({name,button});});
  functionSpecs.forEach(spec=>{
    const button=element('button',spec[1]);
    if(spec[0]==='func.TUNER'){
      button.title='Single click: tuner on/off. Double-click or double-tap: start antenna tuning (transmits a tuning carrier).';
      button.style.touchAction='manipulation';
      button.addEventListener('click',protect(tunerClick));
      button.addEventListener('dblclick',event=>event.preventDefault());
    }else button.addEventListener('click',protect(()=>{const item=setting(spec[0]);return command('set',{setting:spec[0],value:item.value==='0'?'1':'0'});}));
    $('functions').append(button);functionElements.push(button);
  });
}
function render(next) {
  const previous=state;state=next;++stateRevision;const live=state.online&&!state.busy, tx=state.pttKnown&&state.transmitting;
  if(!live||tunerClickGeneration!==state.generation)cancelTunerClick();
  if(previous && previous.generation!==state.generation) { edits.clear();for(const key of [...dirty])if(!preferenceSaver?.versions.has(key))dirty.delete(key);tuningQueue?.clear();stopClientAudio();scopeRows=[];scopeBins=null;scopePeaks=null;scopeGeneration=-1;rfHistory.clear();rfRows=[];rfBins=null; }
  const off=(state.poweredOff||state.standby)&&!state.busy,status=off?'Powered off':state.status;
  const error=off&&state.error?.startsWith('Frequency read:')?'':state.error;
  $('login').hidden=true;$('app').hidden=false;$('app').inert=false;$('mobilePttBar').hidden=false;setText($('connectionStatus'),status+(state.demo?' · DEMO':''));setText($('footerStatus'),error||(off?status:`${status} · ${state.pttKnown?(tx?'TX':'RX'):'TX status unknown'} · ${state.settings.length} driver settings`));
  if(off&&$('notice').textContent.startsWith('Frequency read:'))notify('');
  if(error && error!==previous?.error) notify(error);
  const owned=state.dashboardPtt||state.pttPending;
  $('ptt').classList.toggle('active',owned||state.releasePending);setText($('ptt'),state.releasePending?'Retrying PTT release…':owned?(state.pttPending&&!state.dashboardPtt?'Starting TX…':'PTT off'):tx?'External TX':'PTT on');$('ptt').disabled=!owned&&!state.releasePending&&(!live||!state.canPtt||!state.pttKnown||tx);
  const active=state.vfos[state.activeVfo==='B'?1:0], frequencyText=active.frequencyValid?frequency(active.frequency):'Frequency unavailable';
  setText($('mobileTxStatus'),!state.portOpen?'Connect to radio':state.busy?'Connecting to radio…':off?'Radio powered off':tx?'TRANSMITTING · tap PTT to unkey':`RX · ${frequencyText}`);
  $('powerOn').disabled=!state.portOpen||state.busy||!state.canPower||state.transmitting||state.releasePending;$('powerOff').disabled=!live||!state.canPower;
  const connection=state.connection;
  setText($('connectionSummary'),`${state.portOpen?(off?'Powered off':'Connected'):'Not connected'} · ${connection.endpoint||'Choose an endpoint'}`);
  value('connectionKind',connection.kind);for(const key of ['endpoint','baud','stopBits','flow','poll'])value(key,connection[key]);
  for(const id of ['connectionKind','endpoint','baud','stopBits','flow','poll'])$(id).disabled=state.portOpen||state.busy;
  $('endpoint').disabled=state.portOpen||state.busy||connection.kind===2;$('connectRadio').disabled=state.busy;setText($('connectRadio'),state.portOpen?'Disconnect':state.busy?'Opening…':'Connect');
  if(!dirty.has('shareEnabled'))$('shareEnabled').checked=state.sharing.wanted;value('sharePort',state.sharing.port);$('shareEnabled').disabled=connection.kind===1;$('saveSharing').disabled=connection.kind===1||state.busy;
  setText($('sharingStatus'),state.sharing.error||(state.sharing.running?`127.0.0.1:${state.sharing.port} · ${state.sharing.clients} Hamlib clients`:'CAT sharing inactive'));
  ['A','B'].forEach((name,index)=>{const vfo=state.vfos[index];setText($('freq'+name),vfo.frequencyValid?frequency(vfo.frequency):'--.---.---');setText($('mode'+name),vfo.modeValid?vfo.mode:'--');setText($('width'+name),vfo.modeValid?vfo.width+' Hz filter':'-- Hz filter');setText($('band'+name),vfo.frequencyValid?band(vfo.frequency):'Waiting for radio');$('card'+name).classList.toggle('active',state.online&&state.activeVfo===name);});
  editAcknowledged('controlVfo',state.vfoRevision);value('controlVfo',state.activeVfo);const index=$('controlVfo').value==='B'?1:0,vfo=state.vfos[index];
  editAcknowledged('frequency',vfo.frequencyRevision);editAcknowledged('mode',vfo.modeRevision);editAcknowledged('width',vfo.modeRevision);
  value('frequency',vfo.frequencyValid?(vfo.frequency/1e6).toFixed(6):'');optionList($('mode'),state.modes.map(mode=>[mode,mode]),vfo.modeValid?vfo.mode:'');value('width',vfo.modeValid?vfo.width:'');
  for(const id of ['frequency','mode','width','controlVfo','up','down'])$(id).disabled=!live;
  document.querySelectorAll('[data-vfo],[data-action="swap"],[data-action="copy"]').forEach(node=>node.disabled=!live);
  meterSpecs.forEach(([id,title,format,proportion],index)=>{
    let item=setting(id),raw=Number(item?.value),valid=live&&item?.valid&&item.age<=2000&&(!item.txOnly||tx),note=item?.status||'Unavailable';
    if(index===1&&!item){item=setting('level.RFPOWER_METER');raw=Number(item?.value)*100;valid=live&&item?.valid&&item.age<=2000&&tx;}
    if(!Number.isFinite(raw))valid=false;if(item?.txOnly&&!tx)note=state.pttKnown?'TX only':'TX status unavailable';else if(item?.age>2000)note='Stale / waiting for CAT';
    if(index===1){const power=setting('level.RFPOWER');if(power?.valid)note=(valid?'Measured · set ':'TX only · set ')+Math.round(Number(power.value)*100)+' W';}
    if(index===3&&valid)note='ALC reference scale';if(index===0&&valid)note=raw+' dB relative to S9';
    const elements=meterElements[index];setText(elements.output,valid?format(raw):'--');setText(elements.note,note);elements.fill.style.width=(valid?Math.max(0,Math.min(1,proportion(raw)))*100:0)+'%';elements.output.style.color=index===2&&raw>=3&&valid?'var(--red)':index===2&&raw>=2&&valid?'var(--amber)':'var(--mint)';
  });
  gainSpecs.forEach(([id,label,scale],index)=>{const item=setting(id),elements=gainElements[index];editAcknowledged(elements.input.id,item?.revision||0);if(item?.valid){const raw=Number(item.value)*scale;value(elements.input.id,scale===100?Math.round(raw):raw);setText(elements.readback,'Radio: '+(scale===100?Math.round(raw):raw)+(index===2?' W':index===5?' Hz':' %'));}else{value(elements.input.id,'');setText(elements.readback,'Radio: --');}elements.input.disabled=elements.button.disabled=!live||!item?.valid||!item.writable;});
  setText($('gainVfo'),'RF power / meters: shared · Gains: '+state.settingsVfo);
  cycleElements.forEach(({name,button})=>{const current=cycleControlState(name);setText(button,current.label);button.disabled=!live||!current.enabled;button.classList.toggle('selected',current.selected);});
  functionSpecs.forEach(([id],index)=>{const item=setting(id),button=functionElements[index];button.disabled=!live||!item?.valid||!item.writable;button.classList.toggle('selected',!!item?.valid&&item.value!=='0');});
  value('settingsVfo',state.settingsVfo);if(activePage==='settings')renderSettings();renderAudio();
  const log=state.activity.join('\n'),pre=$('activityLog'),bottom=pre.scrollHeight-pre.scrollTop-pre.clientHeight<30;if(pre.textContent!==log){pre.textContent=log;if(bottom)pre.scrollTop=pre.scrollHeight;}
  $('catQuery').disabled=$('catWrite').disabled=!live||state.demo||connection.kind!==0;
  setText($('scopeStatus'),state.scope.generated?'DEMO / GENERATED AUDIO · no radio audio device is open':!state.online?'Connect the radio and enable Receive audio':tx?'Paused during TX · earlier waterfall history retained':state.scope.fresh?'Live receive audio · source: '+(state.audio.devices.find(device=>device.id===state.audio.maps[0])?.name||'radio receive'):'Waiting for receive samples · '+state.audio.receiveText);
  const rf=state.rfScope||{};
  const rfStatus=rf.status||'RF capture is off';if(previous?.rfScope&& !rf.running&&rfStatus!==previous.rfScope.status&&!/stopped|off/i.test(rfStatus))notify('RF capture: '+rfStatus);
  setText($('rfStatus'),rfStatus);updateRfCaptureButtons(rf);
  if(rf.requestedSpanIndex>=0&&rf.requestedSpanIndex<=9&&!dirty.has('rfSpan')&&document.activeElement!==$('rfSpan'))$('rfSpan').value=String(rf.requestedSpanIndex);
  if(rf.cat2Port!==undefined&&!dirty.has('rfCat2Port')&&document.activeElement!==$('rfCat2Port'))$('rfCat2Port').value=rf.cat2Port;
  if(rf.cat2Baud!==undefined&&!dirty.has('rfCat2Baud')&&document.activeElement!==$('rfCat2Baud'))$('rfCat2Baud').value=rf.cat2Baud;
  if(rf.levelDb!==undefined&&!dirty.has('rfWaterfallLevel')&&document.activeElement!==$('rfWaterfallLevel'))$('rfWaterfallLevel').value=Number(rf.levelDb).toFixed(1);
  $('rfApplySpan').disabled=!!rf.applying;setText($('rfApplyStatus'),rf.applyStatus||'');scheduleScopes();
  if(microphone&&(!state.online||!state.audio.microphoneWanted||state.audio.maps[2]!=='browser:capture'))stopMicrophone();
  audioNode?.port.postMessage({kind:'gate',enabled:!!(microphone&&tx&&state.dashboardPtt&&state.browserOwner===client),lease:state.micLease||0});
  audioNode?.port.postMessage({kind:'listen',enabled:listening&&!tx&&state.online&&state.pttKnown});
  if(!state.scope.fresh&&!scopeFrozen)scopeBins=null;scheduleScopes();updateDiagnostics();resumeMicrophoneWhenReady();
}
function renderSettings() {
  const query=$('settingFilter').value.toLowerCase(),alive=new Set();let count=0;
  for(const item of state.settings){alive.add(item.id);let row=settingRows.get(item.id);if(!row){row=element('tr');for(let i=0;i<5;i++)row.append(element('td'));const cell=element('td'),button=element('button','Edit'),access=element('span','Read only');button.addEventListener('click',()=>openSetting(item.id));cell.append(button,access);row.append(cell);row.editButton=button;row.accessLabel=access;row.addEventListener('dblclick',()=>openSetting(item.id));settingRows.set(item.id,row);$('settingRows').append(row);}const cells=row.children;setText(cells[0],item.name);setText(cells[1],item.group);setText(cells[2],item.valid?item.display:'--');setText(cells[3],item.range);setText(cells[4],item.status+(item.valid?` · ${(item.age/1000).toFixed(1)} s`:''));row.editButton.disabled=!state.online||state.busy||!item.writable;row.editButton.hidden=!item.writable;row.accessLabel.hidden=item.writable;row.hidden=!!query&&!`${item.id} ${item.name} ${item.group}`.toLowerCase().includes(query);if(!row.hidden)count++;}
  for(const [id,row]of settingRows)if(!alive.has(id)){row.remove();settingRows.delete(id);}setText($('settingCount'),`${count} / ${state.settings.length} settings`);
}
function openSetting(id) {
  const item=setting(id);if(!item?.writable||!state.online)return;selectedSetting=id;setText($('settingTitle'),item.name);setText($('settingDescription'),item.description);setText($('settingReadback'),'Radio: '+(item.valid?item.display:'unavailable'));setText($('settingRange'),item.range+' · '+item.units);
  const choices=item.type==='boolean'?[['Off','0'],['On','1']]:item.choices.map((label,i)=>[label,item.choiceValues[i]??label]);$('settingChoices').hidden=!choices.length;$('settingValue').hidden=!!choices.length;
  if(choices.length)optionList($('settingChoices'),choices,item.valid?item.value:choices[0][1]);else{$('settingValue').value=item.valid?item.value:'';$('settingValue').type=item.type==='text'?'text':'number';$('settingValue').step=item.step||'any';if(item.bounded){$('settingValue').min=item.minimum;$('settingValue').max=item.maximum;}else{$('settingValue').removeAttribute('min');$('settingValue').removeAttribute('max');}}
  $('settingDialog').showModal();
}
function renderAudio() {
  const a=state.audio;setText($('audioError'),a.error);
  $('unattended').checked=!!a.unattended;
  setText($('hostReceiveState'),a.receiveWanted?'On':'Off');setText($('hostMicrophoneState'),a.microphoneWanted?'On':'Off');
  for(let slot=0;slot<4;slot++){const input=slot===0||slot===2,radio=slot===0||slot===3;const devices=a.devices.filter(device=>device.input===input&&(!radio||!device.remote));const entries=[['Choose a device',''],...devices.map(device=>[device.name,device.id])];if(a.maps[slot]&&!devices.some(device=>device.id===a.maps[slot]))entries.push(['Saved device unavailable / reconnect',a.maps[slot]]);optionList($('audioMap'+slot),entries,a.maps[slot]);}
  for(const [id,wanted,label]of [['receiveRoute',a.receiveWanted,'Receive audio'],['scopeReceive',a.receiveWanted,'Receive audio'],['microphoneRoute',a.microphoneWanted,'Microphone']]){setText($(id),label+': '+(wanted?'On':'Off'));$(id).classList.toggle('selected',wanted);$(id).disabled=!wanted&&(!state.online||state.demo||state.busy);}
  setText($('receiveStatus'),a.receiveText);setText($('microphoneStatus'),a.microphoneText);value('receiveLevel',a.receiveLevel);value('microphoneLevel',a.microphoneLevel);value('pttLimit',a.pttLimit);
  for(const [id,amount]of [['receiveInput',a.inputPeak],['receiveOutput',a.outputPeak],['microphoneInput',a.micInputPeak],['microphoneOutput',a.micOutputPeak]])$(id).value=Math.min(1,Math.max(0,amount));
  $('listen').classList.toggle('selected',listening);setText($('listen'),listening?'Stop listening':'Listen in this browser');$('listen').disabled=listeningBusy;$('microphone').classList.toggle('selected',microphone);setText($('microphone'),microphone?'Disarm browser microphone':!browserMicAvailable()?'Browser microphone unavailable':microphoneBusy?'Waiting for microphone…':'Arm browser microphone');$('microphone').disabled=microphoneBusy||!browserMicAvailable();
  $('playbackMicWorkaround').disabled=!navigator.audioSession||microphone||microphoneBusy;
  $('stableAudio').disabled=!navigator.audioSession||$('playbackMicWorkaround').checked;
  renderMicrophoneMeter();
}
function connectionLost(connection,reason){
  if(socket!==connection)return;
  socket=null;lastDisconnect=reason;cancelTunerClick();clearTimeout(connectionTimer);clearInterval(heartbeat);
  if(document.hidden)rememberMicrophoneReturn();
  if(microphoneReturn){microphoneReturn.probe='';microphoneReturn.ready=false;}
  tuningQueue?.clear();stopClientAudio(!!microphoneReturn);
  for(const item of pending.values()){clearTimeout(item.timer);item.reject(new Error('Station connection lost'));}
  pending.clear();edits.clear();client=0;
  $('ptt').disabled=$('powerOn').disabled=$('powerOff').disabled=true;$('app').inert=true;
  setText($('connectionStatus'),'Browser disconnected · readings paused');
  setText($('footerStatus'),'Connection lost · controls paused · browser audio stopped');
  if(!scopeFrozen){scopeBins=null;scheduleScopes();}
  if(!closing){notify('Station connection lost. Browser audio stopped; reconnecting…');reconnect();}
}
function retireConnection(connection,reason){
  connectionLost(connection,reason);
  try{connection.close(4000,reason);}catch{}
}
function reconnect() {
  if(closing)return;clearTimeout(reconnectTimer);
  const revision=connectionRevision;
  reconnectTimer=setTimeout(async()=>{
    if(closing||revision!==connectionRevision||(socket&&socket.readyState<2))return;
    const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),6000);
    try{
      const reply=await fetch('/api/ping',{signal:controller.signal,cache:'no-store'});
      if(closing||revision!==connectionRevision||(socket&&socket.readyState<2))return;
      if(reply.status===401){microphoneReturn=null;$('login').hidden=false;$('app').hidden=true;$('mobilePttBar').hidden=true;return;}
      if(!reply.ok)throw new Error('Station unavailable');
      connectWeb();
    }catch{if(!closing&&revision===connectionRevision){reconnectDelay=Math.min(10000,reconnectDelay*1.5);reconnect();}}
    finally{clearTimeout(timeout);}
  },reconnectDelay);
}
async function connectWeb() {
  if(closing)return;
  clearTimeout(reconnectTimer);clearTimeout(connectionTimer);clearInterval(heartbeat);++connectionRevision;
  const previous=socket;if(previous)retireConnection(previous,'Connection replaced');clearTimeout(reconnectTimer);
  let receivedState=false,delayedTick=false,lastReply=performance.now(),lastTick=lastReply,connection;
  try{connection=new WebSocket(`wss://${location.host}/ws`);}catch{
    lastDisconnect='Cannot open station WebSocket';reconnectDelay=Math.min(10000,reconnectDelay*1.5);reconnect();return;
  }
  socket=connection;socket.binaryType='arraybuffer';
  connectionTimer=setTimeout(()=>{if(socket===connection)retireConnection(connection,'Station handshake timed out');},8000);
  const keepAlive=()=>{
    if(socket!==connection||connection.readyState!==1)return;
    const now=performance.now();
    // Forgive one delayed JavaScript tick, then require a fresh server reply.
    // Native WebSocket pongs cover rendering pauses without relying on this timer.
    if(now-lastTick>6000&&!delayedTick){lastReply=now;delayedTick=true;}
    lastTick=now;
    if(now-lastReply>15000){retireConnection(connection,'Station heartbeat reply timed out');return;}
    if(connection.bufferedAmount>32768)return;
    try{connection.send('action=heartbeat');}catch{retireConnection(connection,'Station heartbeat send failed');}
  };
  connection.onopen=()=>{if(socket!==connection)return;reconnectDelay=2000;lastReply=lastTick=performance.now();keepAlive();heartbeat=setInterval(keepAlive,2000);};
  connection.onmessage=event=>{
    if(socket!==connection)return;
    if(typeof event.data!=='string'){binary(event.data);return;}
    let data;
    try{data=JSON.parse(event.data);}catch{retireConnection(connection,'Invalid station reply');return;}
    if(data.type==='heartbeat'){
      lastReply=performance.now();delayedTick=false;
      if(microphoneReturn?.visible&&microphoneReturn.probe&&data.resumeMic===microphoneReturn.probe){microphoneReturn.ready=true;microphoneReturn.afterState=stateRevision;}
      return;
    }
    if(data.type==='controlIdle'){
      if(state?.browserOwner===client)command('ptt',{enabled:'0'}).catch(()=>{});
      if(document.hidden)rememberMicrophoneReturn();
      stopMicrophone(!!microphoneReturn);setText($('browserStatus'),microphoneReturn?'Microphone paused; waiting to restore it when the page returns.':'Browser microphone disarmed after a control pause. Arm it again to transmit.');return;
    }
    if(data.type==='delta'){if(!receivedState)return;render(mergeState(state,data));return;}
    if(data.type==='hello'){client=data.client;return;}
    if(data.type==='state'){
      render(data);
      if(!receivedState){receivedState=true;clearTimeout(connectionTimer);lastReply=performance.now();subscribeScopes();preferenceSaver?.flush();}
      return;
    }
    if(data.type==='error'){notify(data.error);return;}
    if(data.type==='ack'){const waiting=pending.get(data.seq);if(!waiting)return;lastCommandMs=performance.now()-waiting.sent;clearTimeout(waiting.timer);pending.delete(data.seq);const edit=edits.get(waiting.editKey);if(data.ok){if(edit?.seq===data.seq)edit.revision=data.revision||0;waiting.resolve(data);}else{if(edit?.seq===data.seq)edits.delete(waiting.editKey);waiting.reject(new Error(data.error));}}
  };
  connection.onclose=event=>connectionLost(connection,'WebSocket closed ('+(event.code||1006)+')'+(event.reason?': '+event.reason:''));
  connection.onerror=()=>{};
}
function disablePlaybackMicWorkaround() {
  $('playbackMicWorkaround').checked=false;playbackMicActive=false;
  try{localStorage.setItem('playbackMicWorkaround','0');}catch{}
}
function audioSessionType(capturing=microphone, preparing=false, listeningNow=listening) {
  const session=navigator.audioSession,workaround=!!$('playbackMicWorkaround')?.checked;
  playbackMicActive=!!session&&capturing&&!preparing&&workaround;
  if(!session)return;
  const record=capturing||(listeningNow&&$('stableAudio')?.checked&&!workaround);
  try{
    session.type=record&&!playbackMicActive?'play-and-record':'playback';
    if(playbackMicActive&&session.type!=='playback')throw new Error('Playback session was not applied');
  }catch{
    if(playbackMicActive){
      disablePlaybackMicWorkaround();
      try{session.type=capturing?'play-and-record':'playback';}catch{}
      notify('Playback-volume workaround unavailable. Using standard microphone audio.');
    }
  }
}
function audioInterrupted(message, checkPlayback=false) {
  if(document.hidden)rememberMicrophoneReturn();
  const returning=!!microphoneReturn;
  if(checkPlayback&&playbackMicActive&&!returning){
    disablePlaybackMicWorkaround();
    message+=' Playback-volume workaround switched off. Arm the microphone again.';
  }
  if(state?.browserOwner===client)command('ptt',{enabled:'0'}).catch(()=>{});
  stopMicrophone(returning);setText($('browserStatus'),returning?'Microphone paused; it will try to resume when you return.':message);$('resumeAudio').hidden=false;updateDiagnostics();
}
function rememberMicrophoneReturn(){
  if(closing)return;
  if(!$('resumeMicOnReturn')?.checked){microphoneReturn=null;return;}
  if(microphone&&state?.online&&!state.demo){
    microphoneReturn={generation:state.generation,output:state.audio.maps[3],input:$('browserInput').value,visible:false,ready:false,probe:'',afterState:stateRevision,audioRequested:false};
  }
}
function microphonePageReturned(){
  if(!microphoneReturn||document.hidden||closing)return;
  microphoneReturn.visible=true;microphoneReturn.ready=false;microphoneReturn.probe='';microphoneReturn.audioRequested=false;
  setText($('browserStatus'),'Checking the connection before restoring the microphone…');resumeMicrophoneWhenReady();
}
function resumeMicrophoneWhenReady(){
  const request=microphoneReturn;
  if(!request||!request.visible||document.hidden||closing||!state||socket?.readyState!==1||!client)return;
  if(!$('resumeMicOnReturn')?.checked||state.generation!==request.generation||!state.online||state.demo||state.audio.maps[2]!=='browser:capture'||state.audio.maps[3]!==request.output||$('browserInput').value!==request.input){
    microphoneReturn=null;setText($('browserStatus'),'Radio or audio mapping changed. Arm the microphone manually.');return;
  }
  if(!request.probe){
    request.probe=String(++microphoneReturnProbe);
    try{socket.send('action=heartbeat&resumeMic='+request.probe);}catch{request.probe='';}
    return;
  }
  if(!request.ready||stateRevision<=request.afterState||microphoneBusy)return;
  if(state.busy||!state.pttKnown||state.transmitting||state.dashboardPtt||state.pttPending||state.releasePending)return;
  if(microphone){microphoneReturn=null;return;}
  if(!audioContext||audioContext.state!=='running'){
    $('resumeAudio').hidden=false;setText($('resumeAudio'),'Resume audio & mic');
    setText($('browserStatus'),'Restoring microphone audio. If Safari pauses it, tap Resume audio & mic.');
    if(audioContext&&!request.audioRequested&&audioContext.state!=='closed'){
      request.audioRequested=true;
      audioContext.resume().then(()=>{if(microphoneReturn===request)resumeMicrophoneWhenReady();}).catch(()=>{});
    }
    return;
  }
  microphoneReturn=null;
  armMicrophone({automatic:true,generation:request.generation,output:request.output,input:request.input}).then(()=>{
    if(microphone){$('resumeAudio').hidden=true;setText($('resumeAudio'),'Resume audio');setText($('browserStatus'),'Microphone restored. PTT is off; tap PTT when ready.');}
  }).catch(error=>{setText($('resumeAudio'),'Resume audio');setText($('browserStatus'),'Microphone could not resume automatically. Tap Arm browser microphone.');notify(error.message||String(error));});
}
function browserPlaybackGain(volume,boostDb){
  const level=Number(volume),boost=Number(boostDb);
  return Math.max(0,Math.min(100,Number.isFinite(level)?level:100))/100*
    Math.pow(10,Math.max(0,Math.min(24,Number.isFinite(boost)?boost:6))/20);
}
function browserPlaybackCurve(){
  const curve=new Float32Array(4097);
  for(let i=0;i<curve.length;i++){
    const x=2*i/(curve.length-1)-1,magnitude=Math.abs(x);
    curve[i]=magnitude<=.85?x:Math.sign(x)*(.85+.15*Math.tanh((magnitude-.85)/.15));
  }
  return curve;
}
function applyBrowserPlaybackLevel(){
  if(listeningGain)listeningGain.gain.value=browserPlaybackGain($('browserVolume').value,$('browserBoostDb').value);
}
function applyBrowserMicrophoneLevel(){
  const gain=browserPlaybackGain(100,$('browserMicBoostDb').value);
  audioNode?.port.postMessage({kind:'micGain',value:gain});
}
function microphoneMeterReading(value){
  const number=Number(value),peak=Number.isFinite(number)?Math.max(0,Math.min(1,number)):0;
  const db=peak>0?20*Math.log10(peak):-Infinity;
  return {db,level:Math.max(0,Math.min(1,(db+72)/72)),text:Number.isFinite(db)?db.toFixed(1)+' dBFS':'Silent'};
}
function renderMicrophoneMeter(){
  const meter=$('clientMicPeak');if(!meter)return;
  const capturing=!!micSource&&!!micStream;
  const fresh=micMeter.seen!==null&&performance.now()-micMeter.seen<1500;
  meter.value=0;
  if(!capturing){setText($('clientMicLevel'),microphoneBusy?'Opening microphone…':'Mic off');setText($('clientMicHint'),'Arm the browser microphone and speak. PTT is not required for this local input meter.');return;}
  if(!fresh||!micMeter.present){setText($('clientMicLevel'),'Waiting for mic samples');setText($('clientMicHint'),audioContext?.state==='running'?'No input samples detected. Check microphone permission and the selected browser microphone.':'Audio is paused. Use Resume audio or arm the microphone again.');return;}
  const boosted=microphoneMeterReading(micMeter.peak),input=microphoneMeterReading(micMeter.input);
  meter.value=boosted.level;
  setText($('clientMicLevel'),'Boosted: '+boosted.text);
  setText($('clientMicHint'),'Phone input: '+input.text+' · '+(micMeter.limited?'Peak protection active — reduce mic gain.':'Input is live · PTT not required.'));
}
function microphoneMeterMessage(data){
  if(!micSource||!micStream)return;
  micMeter={peak:Number(data.value)||0,input:Number(data.inputPeak??data.value)||0,present:data.inputPresent!==false,limited:!!data.limited,seen:performance.now()};
  renderMicrophoneMeter();
}
async function ensureAudio() {
  if(!window.AudioContext)throw new Error('This browser does not support Web Audio playback');
  if(audioContext?.state==='closed'){audioContext=audioSetup=audioNode=listeningGain=null;}
  if(!audioContext){audioContext=new AudioContext({latencyHint:'interactive',sampleRate:48000});const context=audioContext;const resumed=context.resume();audioSetup=(async()=>{await resumed;if(isSecureContext&&window.AudioWorkletNode&&context.audioWorklet){await context.audioWorklet.addModule('/audio-worklet.js');audioNode=new AudioWorkletNode(context,'station-audio',{numberOfInputs:1,numberOfOutputs:1,outputChannelCount:[2]});audioNode.supportsMicrophone=true;}else audioNode=new StationPlayback(context);listeningGain=context.createGain();applyBrowserPlaybackLevel();const peakGuard=context.createWaveShaper();peakGuard.curve=browserPlaybackCurve();audioNode.connect(listeningGain).connect(peakGuard).connect(context.destination);if(isSecureContext&&context.setSinkId&&$('browserOutput').value)await context.setSinkId($('browserOutput').value);audioNode.port.onmessage=({data})=>{if(data.kind==='mic'&&microphone&&socket?.readyState===1&&socket.bufferedAmount<8192)socket.send(data.buffer);if(data.kind==='peak')microphoneMeterMessage(data);};})().catch(async error=>{await context.close().catch(()=>{});if(audioContext===context){audioContext=audioNode=listeningGain=audioSetup=null;}throw error;});}
  await audioSetup;
  audioContext.onstatechange=()=>{updateDiagnostics();if(['interrupted','suspended','closed'].includes(audioContext.state)&&(microphone||microphoneBusy))audioInterrupted('Audio interrupted. Tap Resume audio, then arm the microphone again.',true);if(audioContext.state==='running')resumeMicrophoneWhenReady();};
  await audioContext.resume();
}
function requireAudioRequest(epoch,owner){
  if(epoch!==audioEpoch||owner!==client||closing||socket?.readyState!==1||!state?.online)throw new Error('Connection changed; enable browser audio again');
}
async function ensureReceive(epoch=audioEpoch,owner=client) {
  requireAudioRequest(epoch,owner);
  if(!state.online||state.demo)throw new Error('Connect the physical radio before enabling audio');
  if(!state.audio.maps[0])throw new Error('Choose the radio USB recording device first');
  if(!state.audio.maps[1])await command('audioMap',{slot:'1',device:'browser:playback'});
  requireAudioRequest(epoch,owner);
  if(!state.audio.receiveWanted)await command('audioRoute',{route:'receive',enabled:'1'});
  requireAudioRequest(epoch,owner);
}
async function listen() {
  if(listeningBusy)return;listeningBusy=true;const epoch=audioEpoch,owner=client;renderAudio();
  try{if(listening){listening=false;audioSessionType();audioNode?.port.postMessage({kind:'listen',enabled:false});await command('listen',{enabled:'0'});setText($('browserStatus'),'Listening stopped.');}
  else{audioSessionType(microphone,false,true);await ensureAudio();requireAudioRequest(epoch,owner);await ensureReceive(epoch,owner);requireAudioRequest(epoch,owner);await command('listen',{enabled:'1'});requireAudioRequest(epoch,owner);listening=true;expectedAudio=undefined;audioSessionType();audioNode.port.postMessage({kind:'listen',enabled:!state.transmitting});setText($('browserStatus'),'Listening to radio receive audio in this browser.');}}
  finally{listeningBusy=false;renderAudio();}
}
function stopMicrophone(keepReturn=false) { if(!keepReturn)microphoneReturn=null;++audioEpoch;microphone=false;playbackMicActive=false;audioNode?.port.postMessage({kind:'gate',enabled:false,lease:0});micSource?.disconnect();micSource=null;const stream=micStream;micStream=null;stream?.getTracks().forEach(track=>track.stop());if(micRegistered){micRegistered=false;command('browserMic',{enabled:'0'}).catch(()=>{});}audioSessionType();micMeter={peak:0,input:0,present:false,limited:false,seen:null};renderMicrophoneMeter();if(state)renderAudio(); }
function stopClientAudio(keepReturn=false) { listening=false;stopMicrophone(keepReturn);audioNode?.port.postMessage({kind:'listen',enabled:false});audioNode?.port.postMessage({kind:'reset'});expectedAudio=undefined;setText($('browserStatus'),'Browser audio stopped.');if(state)renderAudio(); }
async function armMicrophone(options={}) {
  const automatic=options?.automatic===true;
  if(!automatic)microphoneReturn=null;
  const mayRestore=()=>!automatic||(!document.hidden&&!closing&&$('resumeMicOnReturn')?.checked&&state?.generation===options.generation&&state.audio.maps[2]==='browser:capture'&&state.audio.maps[3]===options.output&&$('browserInput').value===options.input&&state.pttKnown&&!state.transmitting&&!state.dashboardPtt&&!state.pttPending&&!state.releasePending);
  if(!mayRestore())throw new Error('Wait for confirmed RX before restoring the microphone');
  if(microphone){if(state.browserOwner===client)await command('ptt',{enabled:'0'});stopMicrophone();setText($('browserStatus'),'Browser microphone disarmed.');return;}
  if(!browserMicAvailable())throw new Error('Microphone capture needs a supported browser and trusted HTTPS. Install and trust the station certificate, then open this page in Safari or another current browser.');
  if(!state.online||state.demo||!state.audio.maps[3])throw new Error('Connect the radio and choose its USB transmit playback device first');
  if(microphoneBusy)return;microphoneBusy=true;const epoch=audioEpoch,owner=client;renderAudio();
  try{
    // Request a capture-compatible session before permission/device access.
    // The optional playback override is applied only after capture is live.
    audioSessionType(true,true);await ensureAudio();
    if(epoch!==audioEpoch||owner!==client||!mayRestore())throw new Error('Audio interrupted while restoring the microphone');
    const device=$('browserInput').value;
    const stream=await navigator.mediaDevices.getUserMedia({audio:{...(device?{deviceId:{exact:device}}:{}),channelCount:1,echoCancellation:false,noiseSuppression:false,autoGainControl:false}});
    if(epoch!==audioEpoch||owner!==client||socket?.readyState!==1||!state.online||!mayRestore()){stream.getTracks().forEach(track=>track.stop());throw new Error('Connection changed; arm the microphone again');}
    micStream=stream;
    if(epoch!==audioEpoch||owner!==client||socket?.readyState!==1||!state.online||!mayRestore())throw new Error('Connection changed; arm the microphone again');
    await command('audioMap',{slot:'2',device:'browser:capture'});
    if(epoch!==audioEpoch||owner!==client||socket?.readyState!==1||!state.online||!mayRestore())throw new Error('Connection changed; arm the microphone again');
    await command('browserMic',{enabled:'1'});micRegistered=true;
    if(epoch!==audioEpoch||owner!==client||socket?.readyState!==1||!state.online||!mayRestore())throw new Error('Connection changed; arm the microphone again');
    for(const track of stream.getAudioTracks()){
      track.addEventListener('ended',()=>{if(micStream===stream)audioInterrupted('Microphone disconnected. Choose a device and arm it again.',true);},{once:true});
      track.addEventListener('mute',()=>{if(micStream===stream)audioInterrupted('Microphone interrupted. Tap Resume audio and arm it again.',true);},{once:true});
    }
    await audioContext.resume();
    if(epoch!==audioEpoch||owner!==client||socket?.readyState!==1||!state.online||!mayRestore())throw new Error('Connection changed; arm the microphone again');
    applyBrowserMicrophoneLevel();
    micSource=audioContext.createMediaStreamSource(stream);micSource.connect(audioNode);microphone=true;
    audioSessionType();
    if(!microphone||!mayRestore()||stream.getAudioTracks().some(track=>track.readyState==='ended'||track.muted)){
      if(playbackMicActive)disablePlaybackMicWorkaround();
      throw new Error('Microphone did not remain active. Arm it again with the playback-volume workaround off.');
    }
    audioNode.port.postMessage({kind:'gate',enabled:!automatic&&!!(state.transmitting&&state.dashboardPtt&&state.browserOwner===client),lease:state.micLease||0});
    setText($('browserStatus'),playbackMicActive?'Microphone armed with playback-volume workaround. Check the mic meter before using PTT.':'Microphone armed. Tap PTT on to transmit, then PTT off.');
    browserDevices().catch(error=>notify(error.message));
  }
  catch(error){stopMicrophone(!!microphoneReturn&&epoch!==audioEpoch);throw error;}
  finally{microphoneBusy=false;renderAudio();resumeMicrophoneWhenReady();}
}
async function browserDevices() {
  if(!navigator.mediaDevices?.enumerateDevices)throw new Error('Browser device selection is unavailable. Check HTTPS certificate trust and browser permissions; use the phone or OS controls for its speaker or headset.');const devices=await navigator.mediaDevices.enumerateDevices();
  for(const [id,kind]of [['browserInput','audioinput'],['browserOutput','audiooutput']]){const current=$(id).value;const entries=[['System default',''],...devices.filter(device=>device.kind===kind&&device.deviceId&&device.deviceId!=='default').map((device,index)=>[device.label||`${kind==='audioinput'?'Microphone':'Output'} ${index+1}`,device.deviceId])];if(current&&!entries.some(entry=>entry[1]===current))entries.push(['Saved device unavailable / allow access',current]);optionList($(id),entries,current);}
  const sink=!!(isSecureContext&&(audioContext?.setSinkId||window.AudioContext?.prototype.setSinkId));$('browserOutput').disabled=!sink;setText($('outputSupport'),sink?'Browser output selection is available.':'This browser uses the device’s system audio output. Change the speaker/headset in the phone or OS controls.');
}
function binary(buffer) {
  const header=new DataView(buffer);if(buffer.byteLength<8)return;const type=header.getUint32(0,true);
  if(type===1&&listening&&audioNode){const sequence=header.getUint32(4,true),pcm=new Int16Array(buffer,8),samples=new Float32Array(pcm.length);for(let i=0;i<pcm.length;i++)samples[i]=pcm[i]/32768;const reset=expectedAudio!==undefined&&expectedAudio!==sequence;expectedAudio=(sequence+pcm.length)>>>0;audioNode.port.postMessage({kind:'rx',samples,reset},[samples.buffer]);}
  if(type===2&&buffer.byteLength===12+2049*4){const rate=header.getUint32(4,true),generation=header.getUint32(8,true);if(generation!==scopeGeneration){clearScope();scopeGeneration=generation;}scopeRate=rate;if(!scopeFrozen){const bins=new Float32Array(buffer,12).slice();if(scopeBins)for(let i=0;i<bins.length;i++)bins[i]=10*Math.log10(.55*Math.pow(10,scopeBins[i]/10)+.45*Math.pow(10,bins[i]/10));scopeBins=bins;const raw=new Float32Array(buffer,12).slice();if(scopePeakHold){if(!scopePeaks)scopePeaks=raw.slice();else for(let i=0;i<raw.length;i++)scopePeaks[i]=Math.max(scopePeaks[i],raw[i]);}scopeRows.unshift({bins:raw,time:performance.now()});if(scopeRows.length>300)scopeRows.pop();scheduleScopes();}}
  if(type===4&&buffer.byteLength===20+850){const center=header.getUint32(4,true),span=header.getUint32(8,true),generation=header.getUint32(12,true),seq=header.getUint32(16,true),bins=new Uint8Array(buffer.slice(20));if(rfHistory.push(center,span,generation,seq,bins)){rfCenterHz=center;rfSpanHz=span;rfBins=bins;rfRows=rfHistory.rows;scheduleScopes();}}
}
let scopeRate=48000,scopeGeneration=-1,scopeBins=null,scopePeaks=null,scopeRows=[],scopeFrozen=false,scopePeakHold=false;
let rfCenterHz=0,rfSpanHz=200000,rfBins=null,rfRows=[];
const palette=[[8,16,29],[24,45,116],[24,128,189],[52,213,181],[255,202,83],[255,92,75]];
const rfPalette=[[5,8,30],[27,25,108],[22,79,184],[0,170,216],[37,220,151],[236,231,74],[255,121,35],[255,246,220]];
function lookupPalette(stops,size){const result=new Uint8ClampedArray(size*4);for(let i=0;i<size;i++){const p=i/(size-1)*(stops.length-1),index=Math.min(stops.length-2,Math.floor(p)),part=p-index;for(let c=0;c<3;c++)result[i*4+c]=Math.round(stops[index][c]+part*(stops[index+1][c]-stops[index][c]));result[i*4+3]=255;}return result;}
const audioColors=lookupPalette(palette,1024),rfColors=lookupPalette(rfPalette,256);
function waterfallImage(key,width,height,rows,signature,pixel){
  let cache=waterfallCaches.get(key);const reset=!cache||cache.width!==width||cache.height!==height||cache.signature!==signature;
  if(reset){const canvas=document.createElement('canvas');canvas.width=width;canvas.height=height;cache={canvas,context:canvas.getContext('2d'),width,height,signature};waterfallCaches.set(key,cache);}
  if(!reset&&cache.last===rows[0])return cache.canvas;
  let added=reset?height:rows.indexOf(cache.last);if(added<0)added=height;added=Math.min(height,added);
  if(added<height&&added>0)cache.context.drawImage(cache.canvas,0,0,width,height-added,0,added,width,height-added);
  if(added>0){const pixels=cache.context.createImageData(width,added);for(let y=0;y<added;y++)for(let x=0;x<width;x++)pixel(rows[y],x,width,pixels.data,(y*width+x)*4);cache.context.putImageData(pixels,0,0);}
  cache.last=rows[0];return cache.canvas;
}
function color(fraction){const scale=Math.max(0,Math.min(1,fraction))*5,index=Math.min(4,Math.floor(scale)),part=scale-index;return palette[index].map((channel,i)=>Math.round(channel+part*(palette[index+1][i]-channel)));}
function clearScope(){scopeBins=null;scopePeaks=null;scopeRows=[];scheduleScopes();}
function canvasContext(id){const canvas=$(id),ratio=devicePixelRatio||1,width=canvas.clientWidth,height=canvas.clientHeight;if(!width||!height)return null;if(canvas.width!==Math.round(width*ratio)||canvas.height!==Math.round(height*ratio)){canvas.width=Math.round(width*ratio);canvas.height=Math.round(height*ratio);}const context=canvas.getContext('2d');context.setTransform(ratio,0,0,ratio,0,0);return {context,width,height};}
function drawScope(){
  if($('scope').hidden)return;const top=canvasContext('spectrumCanvas'),bottom=canvasContext('waterfallCanvas');if(!top||!bottom)return;
  const span=Math.min(Number($('scopeSpan').value),scopeRate/2),gain=Number($('scopeGain').value),floor=Number($('scopeFloor').value),maxBin=span*4096/scopeRate;
  function prepare(item){const {context:c,width,height}=item;c.fillStyle='#08101d';c.fillRect(0,0,width,height);c.font='11px system-ui';return {c,x:48,y:8,w:width-65,h:height-35};}
  function axes(plot,db){const {c,x,y,w,h}=plot;c.strokeStyle='#1f3040';c.fillStyle='#8da2b8';c.lineWidth=1;for(let i=0;i<=6;i++){const pos=x+w*i/6;c.beginPath();c.moveTo(pos,y);c.lineTo(pos,y+h);c.stroke();c.textAlign='center';c.fillText((span*i/6>=1000?(span*i/6000).toFixed(span<12000?1:0)+' k':Math.round(span*i/6)),pos,y+h+20);}if(db)for(let i=0;i<=4;i++){const pos=y+h*i/4;c.beginPath();c.moveTo(x,pos);c.lineTo(x+w,pos);c.stroke();c.textAlign='right';c.fillText(String(floor*i/4-gain),x-8,pos+4);}}
  function column(bins,x,width){const begin=Math.min(2048,Math.floor(x*maxBin/width)),end=Math.min(2049,Math.max(begin+1,Math.floor((x+1)*maxBin/width)));let peak=-140;for(let bin=begin;bin<end;bin++)peak=Math.max(peak,bins[bin]);return peak;}
  const spectrum=prepare(top);axes(spectrum,true);
  function trace(bins,stroke,width){if(!bins)return;const {c,x,y,w,h}=spectrum;c.strokeStyle=stroke;c.lineWidth=width;c.beginPath();for(let i=0;i<Math.floor(w);i++){const db=column(bins,i,w),level=Math.max(0,Math.min(1,(db+gain-floor)/-floor)),pos=y+h*(1-level);if(i)c.lineTo(x+i,pos);else c.moveTo(x+i,pos);}c.stroke();}
  if(scopeBins){if(scopePeakHold)trace(scopePeaks,'#ffc165',1);trace(scopeBins,'#50d3c2',2);let strongest=1;for(let i=2;i<=Math.min(2048,Math.floor(maxBin));i++)if(scopeBins[i]>scopeBins[strongest])strongest=i;setText($('spectrumDetail'),`Peak ${Math.round(strongest*scopeRate/4096)} Hz · ${scopeBins[strongest].toFixed(1)} dBFS · ${(scopeRate/4096).toFixed(1)} Hz / bin`);}else{spectrum.c.textAlign='center';spectrum.c.fillStyle='#8da2b8';spectrum.c.fillText('Waiting for receive samples',spectrum.x+spectrum.w/2,spectrum.y+spectrum.h/2);setText($('spectrumDetail'),'4096-point Hann FFT');}
  const waterfall=prepare(bottom),image=waterfallImage('audio',512,300,scopeRows,[span,gain,floor,scopeGeneration].join('/'),(row,x,width,data,offset)=>{const index=row?Math.max(0,Math.min(1023,Math.round((column(row.bins,x,width)+gain-floor)/-floor*1023))):0;data.set(audioColors.subarray(index*4,index*4+4),offset);});
  waterfall.c.drawImage(image,waterfall.x,waterfall.y,waterfall.w,waterfall.h);
  const gradient=waterfall.c.createLinearGradient(0,waterfall.y,0,waterfall.y+waterfall.h);palette.forEach((rgb,i)=>gradient.addColorStop(1-i/5,`rgb(${rgb.join(',')})`));waterfall.c.fillStyle=gradient;waterfall.c.fillRect(waterfall.x-12,waterfall.y,6,waterfall.h);waterfall.c.fillStyle='#8da2b8';waterfall.c.textAlign='right';waterfall.c.fillText(String(-gain),waterfall.x-16,waterfall.y+10);waterfall.c.fillText(String(floor-gain),waterfall.x-16,waterfall.y+waterfall.h);
  axes(waterfall,false);const seconds=scopeRows.length?(scopeRows[0].time-scopeRows.at(-1).time)/1000:0;setText($('waterfallDetail'),`${scopeFrozen?'Frozen':'Newest at top'} · ${scopeRows.length} frames · ${seconds.toFixed(1)} s between first / last`);
}
function drawRfSpectrumCanvas(canvasId,detailId,compact=false){
  const item=canvasContext(canvasId);if(!item)return;
  const center=rfCenterHz||state?.rfScope?.centerHz||state?.vfos?.[state.activeVfo==='B'?1:0]?.frequency||0,span=rfSpanHz||200000;
  const {context:c,width,height}=item;c.fillStyle='#08101d';c.fillRect(0,0,width,height);c.font='11px system-ui';
  const plot={c,x:54,y:8,w:width-70,h:height-35},divisions=compact?2:4;const {x,y,w,h}=plot;
  c.strokeStyle='#1f3040';c.fillStyle='#8da2b8';c.lineWidth=1;
  for(let i=0;i<=divisions;i++){const px=x+w*i/divisions;c.beginPath();c.moveTo(px,y);c.lineTo(px,y+h);c.stroke();c.textAlign=i===0?'left':i===divisions?'right':'center';const hz=center?center-span/2+span*i/divisions:0;c.fillText(center?frequency(hz):`${Math.round((i/divisions-.5)*span/1000)} kHz`,px,y+h+20);}
  for(let i=0;i<=4;i++){const py=y+h*i/4;c.beginPath();c.moveTo(x,py);c.lineTo(x+w,py);c.stroke();}
  if(rfBins){c.strokeStyle='#50d3c2';c.lineWidth=1.5;c.beginPath();for(let i=0;i<rfBins.length;i++){const px=x+w*i/(rfBins.length-1),py=y+h*(1-rfBins[i]/255);if(i)c.lineTo(px,py);else c.moveTo(px,py);}c.stroke();let peak=0;for(let i=1;i<rfBins.length;i++)if(rfBins[i]>rfBins[peak])peak=i;setText($(detailId),`${center?frequency(center)+' center':'Center unavailable'} · ${(span/1000).toLocaleString()} kHz · peak ${((peak/rfBins.length-.5)*span/1000>=0?'+':'')}${((peak/rfBins.length-.5)*span/1000).toFixed(1)} kHz`);}
  else{c.fillStyle='#8da2b8';c.textAlign='center';c.fillText('Waiting for RF scope frames',x+w/2,y+h/2);setText($(detailId),compact?'Tap spectrum to tune · waiting for RF frames':'No FT4222 RF scope frames');}
}
function drawMobileRfPreview(){
  if(!$('dashboard')||$('dashboard').hidden||!window.matchMedia('(max-width: 650px)').matches)return;
  drawRfSpectrumCanvas('mobileRfSpectrumCanvas','mobileRfDetail',true);
  drawRfWaterfallCanvas('mobileRfWaterfallCanvas','mobileRfWaterfallDetail',true);
}
function drawRfWaterfallCanvas(canvasId,detailId,compact=false){
  const item=canvasContext(canvasId);if(!item)return;
  const center=rfCenterHz||state?.rfScope?.centerHz||state?.vfos?.[state.activeVfo==='B'?1:0]?.frequency||0,span=rfSpanHz||200000;
  const {context:c,width,height}=item;c.fillStyle='#08101d';c.fillRect(0,0,width,height);c.font='11px system-ui';const waterfall={c,x:54,y:8,w:width-70,h:height-35};
  // Two CSS pixels per real frame, independent of phone pixel density. This
  // keeps the same scroll speed on desktop and high-DPI mobile displays.
  const historyHeight=Math.max(1,Math.ceil(waterfall.h/2)),rows=Math.min(rfRows.length,historyHeight);
  if(rows){const image=waterfallImage(canvasId,850,historyHeight,rfRows,[rfSpanHz,rfHistory.generation].join('/'),(row,x,width,data,offset)=>{const value=row?row[x]:0;for(let c=0;c<4;c++)data[offset+c]=rfColors[value*4+c];});waterfall.c.drawImage(image,waterfall.x,waterfall.y,waterfall.w,waterfall.h);}
  const divisions=compact?2:4;c.fillStyle='#8da2b8';c.strokeStyle='#1f3040';c.lineWidth=1;
  for(let i=0;i<=divisions;i++){const px=waterfall.x+waterfall.w*i/divisions;c.beginPath();c.moveTo(px,waterfall.y);c.lineTo(px,waterfall.y+waterfall.h);c.stroke();c.textAlign=i===0?'left':i===divisions?'right':'center';const hz=center?center-span/2+span*i/divisions:0;c.fillText(center?frequency(hz):`${Math.round((i/divisions-.5)*span/1000)} kHz`,px,waterfall.y+waterfall.h+20);}
  const fresh=state?.rfScope?.frameValid&&rfHistory.fresh();setText($(detailId),rfRows.length?(fresh?(compact?`2× scroll · ${rows} frames`:`2× scroll · ${rfRows.length} frames · newest at top`):'Frozen history · waiting for fresh RF frames'):(compact?'Waiting for RF frames':'No RF frames received'));
}
function drawRfScope(){
  drawMobileRfPreview();if(!$('rfScope')||$('rfScope').hidden)return;
  drawRfSpectrumCanvas('rfSpectrumCanvas','rfSpectrumDetail');
  drawRfWaterfallCanvas('rfWaterfallCanvas','rfWaterfallDetail');
}
function tuneRfFromCanvas(event,id){
  if(!state?.online||state.busy||!rfBins||!state.rfScope?.frameValid||!rfHistory.fresh())throw new Error('Wait for a fresh RF scope frame before tuning');
  const canvas=$(id),rect=canvas.getBoundingClientRect(),plotLeft=54,plotWidth=Math.max(1,rect.width-70);
  const fraction=Math.max(0,Math.min(1,(event.clientX-rect.left-plotLeft)/plotWidth));
  const activeVfo=state.activeVfo==='B'?1:0,center=rfCenterHz||state.rfScope?.centerHz||state.vfos[activeVfo]?.frequency||0;
  const target=center-rfSpanHz/2+fraction*rfSpanHz;
  if(!Number.isFinite(target)||target<30000||target>60000000)throw new Error('This spectrum position is outside the FT-710 tuning range');
  $('controlVfo').value=state.activeVfo;dirty.delete('frequency');
  return command('frequency',{vfo:state.activeVfo,hz:String(Math.round(target))},'frequency');
}
function installPageLifecycle(){
  window.addEventListener('resize',scheduleScopes);
  window.addEventListener('pagehide',()=>{rememberMicrophoneReturn();if(state?.browserOwner===client)command('ptt',{enabled:'0'}).catch(()=>{});closing=true;++connectionRevision;cancelTunerClick();clearTimeout(connectionTimer);clearInterval(heartbeat);clearTimeout(reconnectTimer);tuningQueue?.clear();stopClientAudio(!!microphoneReturn);socket?.close();for(const item of pending.values()){clearTimeout(item.timer);item.reject(new Error('Page closed'));}pending.clear();edits.clear();});
  window.addEventListener('pageshow',event=>{if(event.persisted){closing=false;connectWeb();setText($('browserStatus'),'Page restored. Enable listening and arm the microphone again.');microphonePageReturned();}});
  const wake=()=>{if(!closing&&(!socket||socket.readyState>1)){reconnectDelay=250;reconnect();}};
  window.addEventListener('online',wake);
  document.addEventListener?.('visibilitychange',()=>{if(!document.hidden){wake();microphonePageReturned();}});
}
async function exportCsv(){if(!state)return;const cell=value=>'"'+String(value??'').replace(/"/g,'""')+'"',utc=new Date().toISOString();const rows=[['Name','Group','Hamlib ID','Value','Raw value','Access','Range','Status','VFO','UTC']];state.vfos.forEach((vfo,index)=>{const name=index?'B':'A';for(const [field,label,valid,reading]of [['frequency','frequency (Hz)',vfo.frequencyValid,String(vfo.frequency)],['mode','mode / width',vfo.modeValid,vfo.mode+' / '+vfo.width]])rows.push(['VFO '+name+' '+label,'Radio / VFO','VFO'+name+'.'+field,valid?reading:'',valid?reading:'','Read / write','',valid?'Live':'Unavailable',name,utc]);});for(const item of state.settings)rows.push([item.name,item.group,item.id,item.valid?item.display:'',item.valid?item.value:'',item.writable?'Read / write':'Read only',item.range,item.status,state.settingsVfo,utc]);const blob=new Blob(['\ufeff'+rows.map(row=>row.map(cell).join(',')).join('\r\n')],{type:'text/csv;charset=utf-8'}),url=URL.createObjectURL(blob),link=element('a');link.href=url;link.download='FT710-settings.csv';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
function setup(){
  preferenceSaver=new PreferenceSaver(readStationPreferences,fields=>command('preferences',fields),key=>dirty.delete(key),(text,failed)=>{setText($('preferenceStatus'),text);$('retryPreferences').hidden=!failed;});
  $('retryPreferences').addEventListener('click',()=>preferenceSaver.flush());
  const phoneLayout=window.matchMedia('(max-width: 650px)');
  if(phoneLayout.matches){$('connectionPanel').open=false;$('hostReceiveRoute').open=false;$('hostMicrophoneRoute').open=false;$('rfControlPanel').open=false;}
  phoneLayout.addEventListener?.('change',event=>{if(!event.matches){$('connectionPanel').open=true;$('hostReceiveRoute').open=true;$('hostMicrophoneRoute').open=true;$('rfControlPanel').open=true;}});
  $('connectionPanel').addEventListener('toggle',()=>$('connectionPanel').querySelector('.connection-toggle').textContent=$('connectionPanel').open?'Hide':'Setup');
  updateStickyNavigation();
  if(window.ResizeObserver){stickyOffsetsObserver=new ResizeObserver(updateStickyNavigation);stickyOffsetsObserver.observe(document.querySelector('.topbar'));stickyOffsetsObserver.observe($('notice'));}
  stickyNoticeObserver=new MutationObserver(updateStickyNavigation);stickyNoticeObserver.observe($('notice'),{childList:true,characterData:true,subtree:true});window.addEventListener('resize',updateStickyNavigation);
  initializeCards();document.querySelectorAll('input,select').forEach(node=>node.addEventListener('input',()=>dirty.add(node.id)));
  for(const id of preferenceIds){$(id).addEventListener('input',saveStationPreferencesSoon);$(id).addEventListener('change',saveStationPreferencesSoon);}
  document.querySelectorAll('[data-page]').forEach(node=>node.addEventListener('click',()=>showPage(node.dataset.page)));
  document.querySelectorAll('[data-action]').forEach(node=>node.addEventListener('click',protect(()=>command(node.dataset.action))));
  document.querySelectorAll('[data-vfo]').forEach(node=>node.addEventListener('click',protect(()=>{$('controlVfo').value=node.dataset.vfo;dirty.delete('frequency');dirty.delete('width');return command('vfo',{vfo:node.dataset.vfo},'controlVfo');})));
  $('loginForm').addEventListener('submit',protect(async event=>{event.preventDefault();const reply=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({code:$('accessCode').value})});const data=await reply.json();if(!reply.ok){$('loginError').textContent=data.error;return;}$('accessCode').value='';microphoneReturn=null;closing=false;connectWeb();}));
  $('connectionForm').addEventListener('submit',protect(event=>{event.preventDefault();clearTimeout(preferenceSaveTimer);if(state.portOpen)return command('disconnect');const fields={kind:$('connectionKind').value};for(const key of ['endpoint','baud','stopBits','flow','poll'])fields[key]=$(key).value;for(const key of ['connectionKind','endpoint','baud','stopBits','flow','poll'])dirty.delete(key);return command('connect',fields);}));
  $('connectionKind').addEventListener('change',()=>{const before=$('endpoint').value;if($('connectionKind').value==='1'&&!before.includes(':'))$('endpoint').value='127.0.0.1:4532';if($('connectionKind').value==='0'&&before.includes(':'))$('endpoint').value='COM3';if(before!==$('endpoint').value){dirty.add('endpoint');preferenceSaver.edit('endpoint');}});
  $('saveSharing').addEventListener('click',protect(async()=>{await command('sharing',{enabled:$('shareEnabled').checked?'1':'0',port:$('sharePort').value});dirty.delete('shareEnabled');dirty.delete('sharePort');}));
  $('ptt').addEventListener('click',protect(()=>command('ptt',{enabled:state.dashboardPtt||state.pttPending?'0':'1'})));
  $('frequencyForm').addEventListener('submit',protect(event=>{event.preventDefault();return command('frequency',{vfo:$('controlVfo').value,hz:String(parseFrequencyInput($('frequency').value))},'frequency');}));
  $('mode').addEventListener('change',protect(async()=>{dirty.delete('width');edits.set('width',{revision:null,until:Date.now()+12000});try{const ack=await command('mode',{vfo:$('controlVfo').value,mode:$('mode').value,width:'0'},'mode');const edit=edits.get('width');if(edit)edit.revision=ack.revision||0;}catch(error){edits.delete('width');throw error;}}));
  $('filterForm').addEventListener('submit',protect(event=>{event.preventDefault();return command('mode',{vfo:$('controlVfo').value,mode:$('mode').value,width:$('width').value},'width');}));
  $('controlVfo').addEventListener('change',protect(()=>{dirty.delete('frequency');dirty.delete('width');return command('vfo',{vfo:$('controlVfo').value},'controlVfo');}));
  const tune=(steps,target=$('controlVfo').value)=>{if(!state?.online||state.busy||!steps)return;dirty.delete('frequency');return command('step',{vfo:target,hz:String(steps*Number($('step').value))},'frequency');};
  $('up').addEventListener('click',protect(()=>tune(1)));$('down').addEventListener('click',protect(()=>tune(-1)));
  tuningQueue=new TuningQueue(async(vfo,hz)=>{if(!state?.online||state.busy)return;dirty.delete('frequency');await command('step',{vfo,hz:String(hz)},'frequency');},90,error=>notify(error.message));
  let drag,lastX,motion=0;const queue=(delta,target=$('controlVfo').value)=>tuningQueue.add(target,delta*Number($('step').value));
  $('dial').addEventListener('pointerdown',event=>{if(!state?.online)return;drag=event.pointerId;lastX=event.clientX;motion=0;$('dial').setPointerCapture(drag);});$('dial').addEventListener('pointermove',event=>{if(drag!==event.pointerId)return;motion+=event.clientX-lastX;lastX=event.clientX;const count=Math.trunc(motion/12);if(count){motion-=count*12;queue(count);}});for(const name of ['pointerup','pointercancel','lostpointercapture'])$('dial').addEventListener(name,()=>{drag=undefined;tuningQueue.flush().catch(error=>notify(error.message));});
  $('dial').addEventListener('keydown',protect(event=>{if(['ArrowLeft','ArrowDown','ArrowRight','ArrowUp','PageUp','PageDown'].includes(event.key)){event.preventDefault();return tune(event.key==='PageUp'?10:event.key==='PageDown'?-10:event.key==='ArrowLeft'||event.key==='ArrowDown'?-1:1);}}));
  for(const id of ['dial','freqA','freqB'])$(id).addEventListener('wheel',event=>{event.preventDefault();queue(event.deltaY<0?1:-1,id==='dial'?$('controlVfo').value:id==='freqB'?'B':'A');},{passive:false});
  $('settingFilter').addEventListener('input',()=>state&&renderSettings());$('settingsVfo').addEventListener('change',protect(async()=>{await command('settingsVfo',{vfo:$('settingsVfo').value});dirty.delete('settingsVfo');}));
  $('closeSetting').addEventListener('click',()=>$('settingDialog').close());$('settingForm').addEventListener('submit',protect(async event=>{event.preventDefault();await command('set',{setting:selectedSetting,value:$('settingChoices').hidden?$('settingValue').value:$('settingChoices').value});$('settingDialog').close();notify('Setting queued. Radio readback appears in the table.',false);}));
  $('clearLog').addEventListener('click',protect(()=>command('clearLog')));$('catPreset').addEventListener('change',()=>$('catCommand').value=$('catPreset').value);$('catForm').addEventListener('submit',protect(event=>{event.preventDefault();return command('rawQuery',{text:$('catCommand').value});}));$('catWrite').addEventListener('click',protect(()=>command('rawWrite',{text:$('catCommand').value})));
  $('audioRefresh').addEventListener('click',protect(()=>command('audioRefresh')));
  for(let slot=0;slot<4;slot++)$('audioMap'+slot).addEventListener('change',protect(async()=>{await command('audioMap',{slot:String(slot),device:$('audioMap'+slot).value});dirty.delete('audioMap'+slot);}));
  for(const id of ['receiveRoute','scopeReceive'])$(id).addEventListener('click',protect(()=>command('audioRoute',{route:'receive',enabled:state.audio.receiveWanted?'0':'1'})));
  $('microphoneRoute').addEventListener('click',protect(()=>command('audioRoute',{route:'microphone',enabled:state.audio.microphoneWanted?'0':'1'})));
  for(const route of ['receive','microphone'])$(route+'Level').addEventListener('change',protect(async()=>{await command('audioLevel',{route,value:$(route+'Level').value});dirty.delete(route+'Level');}));
  $('pttLimit').addEventListener('change',protect(async()=>{await command('pttLimit',{index:$('pttLimit').value});dirty.delete('pttLimit');}));
  $('browserMappings').addEventListener('click',protect(async()=>{await command('audioMap',{slot:'1',device:'browser:playback'});if(browserMicAvailable())await command('audioMap',{slot:'2',device:'browser:capture'});notify('Browser listening selected. Choose the radio USB source, then enable receive audio.',false);}));
  $('listen').addEventListener('click',protect(listen));$('microphone').addEventListener('click',protect(armMicrophone));$('browserDevices').addEventListener('click',protect(browserDevices));
  for(const id of ['browserInput','browserOutput']){const saved=localStorage.getItem(id);if(saved){const option=element('option','Saved device / refresh devices');option.value=saved;$(id).append(option);$(id).value=saved;}}
  const savedBoost=localStorage.getItem('browserBoostDb');if([...$('browserBoostDb').options].some(option=>option.value===savedBoost))$('browserBoostDb').value=savedBoost;
  $('browserBoostDb').addEventListener('change',()=>{localStorage.setItem('browserBoostDb',$('browserBoostDb').value);applyBrowserPlaybackLevel();});
  const savedMicBoost=localStorage.getItem('browserMicBoostDb');if([...$('browserMicBoostDb').options].some(option=>option.value===savedMicBoost))$('browserMicBoostDb').value=savedMicBoost;
  $('browserMicBoostDb').addEventListener('change',()=>{localStorage.setItem('browserMicBoostDb',$('browserMicBoostDb').value);applyBrowserMicrophoneLevel();});
  const savedVolume=localStorage.getItem('browserVolume');if(savedVolume!==null&&Number.isFinite(Number(savedVolume)))$('browserVolume').value=Math.max(0,Math.min(100,Number(savedVolume)));
  $('browserInput').addEventListener('change',()=>{microphoneReturn=null;localStorage.setItem('browserInput',$('browserInput').value);if(microphone||microphoneBusy){stopMicrophone();notify('Microphone selection changed. Arm the browser microphone again.',false);if(state?.browserOwner===client)command('ptt',{enabled:'0'}).catch(()=>{});}});
  $('browserOutput').addEventListener('change',protect(async()=>{await ensureAudio();if(!audioContext.setSinkId)throw new Error('This browser uses the system output');await audioContext.setSinkId($('browserOutput').value);localStorage.setItem('browserOutput',$('browserOutput').value);}));$('browserVolume').addEventListener('input',()=>{localStorage.setItem('browserVolume',$('browserVolume').value);applyBrowserPlaybackLevel();});
  for(const id of ['scopeSpan','scopeGain','scopeFloor']){const saved=localStorage.getItem(id);if([...$(id).options].some(option=>option.value===saved))$(id).value=saved;$(id).addEventListener('change',()=>{localStorage.setItem(id,$(id).value);drawScope();});}
  // Station preferences come from the server; localStorage holds local display/audio choices only.
  const savedRfLevel=localStorage.getItem('rfWaterfallLevel');if(savedRfLevel!==null&&Number.isFinite(Number(savedRfLevel))&&Number(savedRfLevel)>=-30&&Number(savedRfLevel)<=30&&Number.isInteger(Number(savedRfLevel)*2))$('rfWaterfallLevel').value=Number(savedRfLevel).toFixed(1);
  $('rfWaterfallLevel').addEventListener('change',()=>localStorage.setItem('rfWaterfallLevel',Number($('rfWaterfallLevel').value).toFixed(1)));
  for(const id of ['rfCapture','mobileRfCapture'])$(id).addEventListener('click',protect(toggleRfCapture));
  $('rfApplySpan').addEventListener('click',protect(async()=>{const port=$('rfCat2Port').value.trim(),level=Number($('rfWaterfallLevel').value);if(!/^COM\d+$/i.test(port))throw new Error('Enter the radio Standard / CAT-2 COM port, for example COM5');if(!Number.isFinite(level)||level < -30||level > 30||!Number.isInteger(level*2))throw new Error('Waterfall level must be -30 to +30 dB in 0.5 dB steps');await command('rfSpan',{port,baud:$('rfCat2Baud').value,span:$('rfSpan').value,level:level.toFixed(1)});dirty.delete('rfSpan');dirty.delete('rfWaterfallLevel');localStorage.setItem('rfWaterfallLevel',level.toFixed(1));notify('Radio confirmed CENTER mode, RF span, and waterfall level.',false);}));
  $('rfClear').addEventListener('click',()=>{rfHistory.clear();rfRows=[];rfBins=null;waterfallCaches.clear();scheduleScopes();});
  for(const id of ['rfSpectrumCanvas','rfWaterfallCanvas','mobileRfSpectrumCanvas','mobileRfWaterfallCanvas'])$(id).addEventListener('click',protect(event=>tuneRfFromCanvas(event,id)));
  const savedStep=localStorage.getItem('step');if([...$('step').options].some(option=>option.value===savedStep))$('step').value=savedStep;$('step').addEventListener('change',()=>localStorage.setItem('step',$('step').value));
  $('scopeFreeze').addEventListener('click',()=>{scopeFrozen=!scopeFrozen;setText($('scopeFreeze'),scopeFrozen?'Resume':'Freeze');$('scopeFreeze').classList.toggle('selected',scopeFrozen);drawScope();});$('scopePeak').addEventListener('click',()=>{scopePeakHold=!scopePeakHold;scopePeaks=null;setText($('scopePeak'),'Peak hold: '+(scopePeakHold?'On':'Off'));$('scopePeak').classList.toggle('selected',scopePeakHold);drawScope();});$('scopeClear').addEventListener('click',clearScope);
  $('logout').addEventListener('click',protect(async()=>{if(state?.browserOwner===client)await command('ptt',{enabled:'0'});closing=true;++connectionRevision;cancelTunerClick();clearTimeout(connectionTimer);clearTimeout(reconnectTimer);socket?.close();stopClientAudio();await fetch('/api/logout',{method:'POST'});$('app').hidden=true;$('mobilePttBar').hidden=true;$('login').hidden=false;}));
  const stable=localStorage.getItem('stableAudio');$('stableAudio').checked=stable!=='0';$('stableAudio').disabled=!navigator.audioSession;
  $('stableAudio').addEventListener('change',()=>{localStorage.setItem('stableAudio',$('stableAudio').checked?'1':'0');audioSessionType(microphone);});
  $('playbackMicWorkaround').checked=!!navigator.audioSession&&localStorage.getItem('playbackMicWorkaround')==='1';
  $('playbackMicWorkaround').disabled=!navigator.audioSession;
  $('stableAudio').disabled=!navigator.audioSession||$('playbackMicWorkaround').checked;
  $('playbackMicWorkaround').addEventListener('change',()=>{localStorage.setItem('playbackMicWorkaround',$('playbackMicWorkaround').checked?'1':'0');audioSessionType();if(state)renderAudio();});
  $('resumeMicOnReturn').checked=localStorage.getItem('resumeMicOnReturn')==='1';
  $('resumeMicOnReturn').addEventListener('change',()=>{localStorage.setItem('resumeMicOnReturn',$('resumeMicOnReturn').checked?'1':'0');if(!$('resumeMicOnReturn').checked)microphoneReturn=null;});
  $('resumeAudio').addEventListener('click',protect(async()=>{await ensureAudio();$('resumeAudio').hidden=true;setText($('resumeAudio'),'Resume audio');if(microphoneReturn)resumeMicrophoneWhenReady();else setText($('browserStatus'),microphone?'Microphone is ready. Tap PTT when needed.':'Audio resumed. Arm the microphone again to transmit.');}));
  $('unattended').addEventListener('change',protect(()=>command('unattended',{enabled:$('unattended').checked?'1':'0'})));
  navigator.mediaDevices?.addEventListener?.('devicechange',()=>{browserDevices().catch(()=>{});updateDiagnostics();});
  navigator.audioSession?.addEventListener?.('statechange',()=>{if(navigator.audioSession.state==='interrupted')audioInterrupted('Phone audio interrupted. Tap Resume audio.',true);});
  document.addEventListener('visibilitychange',()=>{if(document.hidden)cancelTunerClick();subscribeScopes();if(document.hidden&&(microphone||microphoneBusy))audioInterrupted('Microphone disarmed while the page was hidden. Arm it again to transmit.');else scheduleScopes();});
  installPageLifecycle();
  setInterval(()=>setText($('clock'),new Date().toISOString().slice(11,19)+' UTC'),1000);
  const code=new URLSearchParams(location.hash.slice(1)).get('code');if(code){$('accessCode').value=code;history.replaceState(null,'',location.pathname);}
  fetch('/api/state').then(reply=>{if(reply.ok){closing=false;connectWeb();}}).catch(()=>{});
  const sink=!!(isSecureContext&&window.AudioContext?.prototype.setSinkId);$('browserOutput').disabled=!sink;$('browserInput').disabled=$('browserDevices').disabled=!navigator.mediaDevices?.enumerateDevices;
  setText($('outputSupport'),sink?'Browser output selection is available.':'This browser uses the device’s system audio output.');
  if(navigator.mediaDevices?.enumerateDevices)browserDevices().catch(()=>{});
  const support=element('p',browserMicAvailable()?'Tap Arm browser microphone and allow microphone access. On iPhone, select the speaker or headset using the phone’s audio controls.':'Microphone capture needs a supported browser and trusted HTTPS. Install and trust the station certificate before opening the dashboard.','muted');$('outputSupport').after(support);$('microphone').disabled=!browserMicAvailable();if(!browserMicAvailable())setText($('browserMappings'),'Use browser receive mapping');
}
setup();
