'use strict';
const $ = id => document.getElementById(id), dirty = new Set(), pending = new Map(), edits = new Map();
let state, socket, client = 0, sequence = 0, closing = false, reconnectTimer, reconnectDelay = 2000, heartbeat, noticeTimer, preferenceSaveTimer, selectedSetting, stickyOffsetsObserver, stickyNoticeObserver;
let audioContext, audioSetup, audioNode, listeningGain, micStream, micSource, listening = false, microphone = false, expectedAudio;
let audioEpoch = 0, microphoneBusy = false, listeningBusy = false, rfCaptureAction = '', rfCaptureTimer, rfCaptureBaselineStatus = '';
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
    pending.set(seq,{resolve,reject,timer,editKey}); if(editKey) edits.set(editKey,{seq,revision:null,until:Date.now()+12000}); socket.send(payload.toString());
  });
}
const preferenceIds=['connectionKind','endpoint','baud','stopBits','flow','poll','rfSpan','rfCat2Port','rfCat2Baud','rfWaterfallLevel'];
function saveStationPreferencesSoon() {
  preferenceIds.forEach(id=>dirty.add(id));
  clearTimeout(preferenceSaveTimer);
  preferenceSaveTimer=setTimeout(async()=>{
    const fields={kind:$('connectionKind').value,endpoint:$('endpoint').value};
    for(const key of ['baud','stopBits','flow','poll'])fields[key]=$(key).value;
    fields.rfSpan=$('rfSpan').value;fields.rfCat2Port=$('rfCat2Port').value.trim();fields.rfCat2Baud=$('rfCat2Baud').value;
    const levelText=$('rfWaterfallLevel').value.trim(),level=Number(levelText);
    if(levelText!==''&&Number.isFinite(level)&&level>=-30&&level<=30&&Number.isInteger(level*2))fields.rfWaterfallLevel=level.toFixed(1);
    try { await command('preferences',fields);preferenceIds.forEach(id=>dirty.delete(id)); }
    catch(error) { notify('Could not save settings: '+(error.message||String(error))); }
  },350);
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
  const enabled=functionItem.value!=='0',amount=discreteLevel(level,maximum,dnr),label=enabled?(amount<0?'--':String(amount)):'Off';
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
function showPage(name) { document.querySelectorAll('.page').forEach(node=>node.hidden=node.id!==name);document.querySelectorAll('[data-page]').forEach(node=>node.classList.toggle('selected',node.dataset.page===name));if(name==='scope') drawScope();if(name==='rfScope'||name==='dashboard') drawRfScope();const tab=document.querySelector(`[data-page="${name}"]`);if(tab&&window.matchMedia('(max-width: 650px)').matches)tab.scrollIntoView({behavior:'smooth',block:'nearest',inline:'center'});window.scrollTo(0,0); }
function updateStickyNavigation(){const header=document.querySelector('.topbar'),notice=$('notice');if(!header||!notice)return;const headerHeight=Math.ceil(header.getBoundingClientRect().height),noticeHeight=notice.getBoundingClientRect().height;document.documentElement.style.setProperty('--topbar-height',headerHeight+'px');document.documentElement.style.setProperty('--section-tabs-top',headerHeight+noticeHeight+'px');}
function frequency(hz) { return Math.round(hz).toString().padStart(8,'0').replace(/(\d+)(\d{3})(\d{3})$/,'$1.$2.$3'); }
function parseFrequencyInput(input){let text=String(input).trim().toLowerCase().replace(/\s+/g,'');let multiplier=0;for(const [unit,scale]of [['mhz',1e6],['khz',1e3],['hz',1]])if(text.endsWith(unit)){text=text.slice(0,-unit.length);multiplier=scale;break;}const separators=(text.match(/[.,]/g)||[]).length;if(separators>1)text=text.replace(/[.,]/g,'');else text=text.replace(',','.');const match=text.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))$/);if(!match)throw new Error('Enter a frequency such as 7.074 MHz, 7074 kHz, or 7074000 Hz');const value=Number(match[1]);if(!Number.isFinite(value))throw new Error('Enter a valid radio frequency');if(!multiplier)multiplier=value<=60?1e6:value<=60000?1e3:1;const hz=Math.round(value*multiplier);if(hz<30000||hz>60000000)throw new Error('Frequency must be between 0.030 and 60 MHz');return hz;}
function band(hz) { for(const [lo,hi,name] of [[1.8e6,2e6,'160 m'],[3.5e6,4e6,'80 m'],[5.25e6,5.45e6,'60 m'],[7e6,7.3e6,'40 m'],[10.1e6,10.15e6,'30 m'],[14e6,14.35e6,'20 m'],[18.068e6,18.168e6,'17 m'],[21e6,21.45e6,'15 m'],[24.89e6,24.99e6,'12 m'],[28e6,29.7e6,'10 m'],[50e6,54e6,'6 m']]) if(hz>=lo&&hz<=hi)return name;return 'General coverage'; }
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
  functionSpecs.forEach(spec=>{const button=element('button',spec[1]);button.addEventListener('click',protect(()=>{const item=setting(spec[0]);return command('set',{setting:spec[0],value:item.value==='0'?'1':'0'});}));$('functions').append(button);functionElements.push(button);});
}
function render(next) {
  const previous=state;state=next;const live=state.online&&!state.busy, tx=state.pttKnown&&state.transmitting;
  if(previous && previous.generation!==state.generation) { edits.clear();dirty.clear();stopClientAudio();scopeRows=[];scopeBins=null;scopePeaks=null;scopeGeneration=-1;rfRows=[];rfBins=null; }
  const off=(state.poweredOff||state.standby)&&!state.busy,status=off?'Powered off':state.status;
  const error=off&&state.error?.startsWith('Frequency read:')?'':state.error;
  $('login').hidden=true;$('app').hidden=false;$('app').inert=false;$('mobilePttBar').hidden=false;setText($('connectionStatus'),status+(state.demo?' · DEMO':''));setText($('footerStatus'),error||(off?status:`${status} · ${state.pttKnown?(tx?'TX':'RX'):'TX status unknown'} · ${state.settings.length} driver settings`));
  if(off&&$('notice').textContent.startsWith('Frequency read:'))notify('');
  if(error && error!==previous?.error) notify(error);
  const owned=state.dashboardPtt||state.pttPending;
  $('ptt').classList.toggle('active',owned);setText($('ptt'),owned?(state.pttPending&&!state.dashboardPtt?'Starting TX…':'PTT off'):tx?'External TX':'PTT on');$('ptt').disabled=!owned&&(!live||!state.canPtt||!state.pttKnown||tx);
  const active=state.vfos[state.activeVfo==='B'?1:0], frequencyText=active.frequencyValid?frequency(active.frequency):'Frequency unavailable';
  setText($('mobileTxStatus'),!state.portOpen?'Connect to radio':state.busy?'Connecting to radio…':off?'Radio powered off':tx?'TRANSMITTING · tap PTT to unkey':`RX · ${frequencyText}`);
  $('powerOn').disabled=!state.portOpen||state.busy||!state.canPower;$('powerOff').disabled=!live||!state.canPower;
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
  value('settingsVfo',state.settingsVfo);renderSettings();renderAudio();
  const log=state.activity.join('\n'),pre=$('activityLog'),bottom=pre.scrollHeight-pre.scrollTop-pre.clientHeight<30;if(pre.textContent!==log){pre.textContent=log;if(bottom)pre.scrollTop=pre.scrollHeight;}
  $('catQuery').disabled=$('catWrite').disabled=!live||state.demo||connection.kind!==0;
  setText($('scopeStatus'),state.scope.generated?'DEMO / GENERATED AUDIO · no radio audio device is open':!state.online?'Connect the radio and enable Receive audio':tx?'Paused during TX · earlier waterfall history retained':state.scope.fresh?'Live receive audio · source: '+(state.audio.devices.find(device=>device.id===state.audio.maps[0])?.name||'radio receive'):'Waiting for receive samples · '+state.audio.receiveText);
  const rf=state.rfScope||{};
  const rfStatus=rf.status||'RF capture is off';if(previous?.rfScope&& !rf.running&&rfStatus!==previous.rfScope.status&&!/stopped|off/i.test(rfStatus))notify('RF capture: '+rfStatus);
  setText($('rfStatus'),rfStatus);updateRfCaptureButtons(rf);
  if(rf.spanIndex>=0&&rf.spanIndex<=9&&!dirty.has('rfSpan')&&document.activeElement!==$('rfSpan'))$('rfSpan').value=String(rf.spanIndex);
  if(rf.cat2Port!==undefined&&!dirty.has('rfCat2Port')&&document.activeElement!==$('rfCat2Port'))$('rfCat2Port').value=rf.cat2Port;
  if(rf.cat2Baud!==undefined&&!dirty.has('rfCat2Baud')&&document.activeElement!==$('rfCat2Baud'))$('rfCat2Baud').value=rf.cat2Baud;
  if(rf.levelDb!==undefined&&!dirty.has('rfWaterfallLevel')&&document.activeElement!==$('rfWaterfallLevel'))$('rfWaterfallLevel').value=Number(rf.levelDb).toFixed(1);
  $('rfApplySpan').disabled=false;drawRfScope();
  if(microphone&&(!state.online||!state.audio.microphoneWanted||state.audio.maps[2]!=='browser:capture'))stopMicrophone();
  audioNode?.port.postMessage({kind:'gate',enabled:!!(microphone&&tx&&state.dashboardPtt&&state.browserOwner===client),lease:state.micLease||0});
  audioNode?.port.postMessage({kind:'listen',enabled:listening&&!tx&&state.online&&state.pttKnown});
  if(!state.scope.fresh&&!scopeFrozen)scopeBins=null;drawScope();
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
  setText($('hostReceiveState'),a.receiveWanted?'On':'Off');setText($('hostMicrophoneState'),a.microphoneWanted?'On':'Off');
  for(let slot=0;slot<4;slot++){const input=slot===0||slot===2,radio=slot===0||slot===3;const devices=a.devices.filter(device=>device.input===input&&(!radio||!device.remote));const entries=[['Choose a device',''],...devices.map(device=>[device.name,device.id])];if(a.maps[slot]&&!devices.some(device=>device.id===a.maps[slot]))entries.push(['Saved device unavailable / reconnect',a.maps[slot]]);optionList($('audioMap'+slot),entries,a.maps[slot]);}
  for(const [id,wanted,label]of [['receiveRoute',a.receiveWanted,'Receive audio'],['scopeReceive',a.receiveWanted,'Receive audio'],['microphoneRoute',a.microphoneWanted,'Microphone']]){setText($(id),label+': '+(wanted?'On':'Off'));$(id).classList.toggle('selected',wanted);$(id).disabled=!wanted&&(!state.online||state.demo||state.busy);}
  setText($('receiveStatus'),a.receiveText);setText($('microphoneStatus'),a.microphoneText);value('receiveLevel',a.receiveLevel);value('microphoneLevel',a.microphoneLevel);value('pttLimit',a.pttLimit);
  for(const [id,amount]of [['receiveInput',a.inputPeak],['receiveOutput',a.outputPeak],['microphoneInput',a.micInputPeak],['microphoneOutput',a.micOutputPeak]])$(id).value=Math.min(1,Math.max(0,amount));
  $('listen').classList.toggle('selected',listening);setText($('listen'),listening?'Stop listening':'Listen in this browser');$('listen').disabled=listeningBusy;$('microphone').classList.toggle('selected',microphone);setText($('microphone'),microphone?'Disarm browser microphone':!browserMicAvailable()?'Browser microphone unavailable':microphoneBusy?'Waiting for microphone…':'Arm browser microphone');$('microphone').disabled=microphoneBusy||!browserMicAvailable();
}
function reconnect() {
  if(closing)return;clearTimeout(reconnectTimer);
  reconnectTimer=setTimeout(async()=>{try{const reply=await fetch('/api/state',{signal:AbortSignal.timeout(6000)});if(reply.status===401){$('login').hidden=false;$('app').hidden=true;return;}if(!reply.ok)throw new Error('Station unavailable');connectWeb();}catch{reconnectDelay=Math.min(10000,reconnectDelay*1.5);reconnect();}},reconnectDelay);
}
async function connectWeb() {
  if(closing)return;clearTimeout(reconnectTimer);clearInterval(heartbeat);socket=new WebSocket(`wss://${location.host}/ws`);socket.binaryType='arraybuffer';
  socket.onopen=()=>{reconnectDelay=2000;heartbeat=setInterval(()=>{if(socket.readyState===1)socket.send(new URLSearchParams({action:'heartbeat'}).toString());},2000);};
  socket.onmessage=event=>{
    if(typeof event.data!=='string'){binary(event.data);return;}
    const data=JSON.parse(event.data);
    if(data.type==='hello'){client=data.client;return;}if(data.type==='state'){render(data);return;}
    if(data.type==='error'){notify(data.error);return;}
    if(data.type==='ack'){const waiting=pending.get(data.seq);if(!waiting)return;clearTimeout(waiting.timer);pending.delete(data.seq);const edit=edits.get(waiting.editKey);if(data.ok){if(edit?.seq===data.seq)edit.revision=data.revision||0;waiting.resolve(data);}else{if(edit?.seq===data.seq)edits.delete(waiting.editKey);waiting.reject(new Error(data.error));}}
  };
  socket.onclose=()=>{clearInterval(heartbeat);stopClientAudio();for(const item of pending.values()){clearTimeout(item.timer);item.reject(new Error('Station connection lost'));}pending.clear();edits.clear();client=0;$('ptt').disabled=$('powerOn').disabled=$('powerOff').disabled=true;$('app').inert=true;setText($('connectionStatus'),'Browser disconnected · readings paused');setText($('footerStatus'),'Connection lost · controls paused · browser audio stopped');if(!scopeFrozen){scopeBins=null;drawScope();}if(!closing){notify('Station connection lost. Browser audio stopped; reconnecting…');reconnect();}};
  socket.onerror=()=>{};
}
async function ensureAudio() {
  if(!window.AudioContext)throw new Error('This browser does not support Web Audio playback');
  if(!audioContext){audioContext=new AudioContext({latencyHint:'interactive',sampleRate:48000});const context=audioContext;const resumed=context.resume();audioSetup=(async()=>{await resumed;if(isSecureContext&&window.AudioWorkletNode&&context.audioWorklet){await context.audioWorklet.addModule('/audio-worklet.js');audioNode=new AudioWorkletNode(context,'station-audio',{numberOfInputs:1,numberOfOutputs:1,outputChannelCount:[2]});audioNode.supportsMicrophone=true;}else audioNode=new StationPlayback(context);listeningGain=context.createGain();listeningGain.gain.value=Number($('browserVolume').value)/100;audioNode.connect(listeningGain).connect(context.destination);if(isSecureContext&&context.setSinkId&&$('browserOutput').value)await context.setSinkId($('browserOutput').value);audioNode.port.onmessage=({data})=>{if(data.kind==='mic'&&microphone&&socket?.readyState===1&&socket.bufferedAmount<8192)socket.send(data.buffer);if(data.kind==='peak')$('clientMicPeak').value=data.value;};})().catch(async error=>{await context.close().catch(()=>{});if(audioContext===context){audioContext=audioNode=listeningGain=audioSetup=null;}throw error;});}
  await audioSetup;
  await audioContext.resume();
}
async function ensureReceive() {
  if(!state.online||state.demo)throw new Error('Connect the physical radio before enabling audio');
  if(!state.audio.maps[0])throw new Error('Choose the radio USB recording device first');
  if(!state.audio.maps[1])await command('audioMap',{slot:'1',device:'browser:playback'});
  if(!state.audio.receiveWanted)await command('audioRoute',{route:'receive',enabled:'1'});
}
async function listen() {
  if(listeningBusy)return;listeningBusy=true;const epoch=audioEpoch;renderAudio();
  try{if(listening){listening=false;audioNode?.port.postMessage({kind:'listen',enabled:false});await command('listen',{enabled:'0'});setText($('browserStatus'),'Listening stopped.');}
  else{await ensureAudio();await ensureReceive();await command('listen',{enabled:'1'});if(epoch!==audioEpoch||socket?.readyState!==1)throw new Error('Connection changed; enable listening again');listening=true;expectedAudio=undefined;audioNode.port.postMessage({kind:'listen',enabled:!state.transmitting});setText($('browserStatus'),'Listening to radio receive audio in this browser.');}}
  finally{listeningBusy=false;renderAudio();}
}
function stopMicrophone() { ++audioEpoch;microphone=false;audioNode?.port.postMessage({kind:'gate',enabled:false,lease:0});micSource?.disconnect();micSource=null;micStream?.getTracks().forEach(track=>track.stop());micStream=null;$('clientMicPeak').value=0;if(state)renderAudio(); }
function stopClientAudio() { listening=false;stopMicrophone();audioNode?.port.postMessage({kind:'listen',enabled:false});audioNode?.port.postMessage({kind:'reset'});expectedAudio=undefined;setText($('browserStatus'),'Browser audio stopped.');if(state)renderAudio(); }
async function armMicrophone() {
  if(microphone){if(state.browserOwner===client)await command('ptt',{enabled:'0'});stopMicrophone();await command('audioRoute',{route:'microphone',enabled:'0'});setText($('browserStatus'),'Browser microphone disarmed.');return;}
  if(!browserMicAvailable())throw new Error('Microphone capture needs a supported browser and trusted HTTPS. Install and trust the station certificate, then open this page in Safari or another current browser.');
  if(!state.online||state.demo||!state.audio.maps[3])throw new Error('Connect the radio and choose its USB transmit playback device first');
  if(microphoneBusy)return;microphoneBusy=true;const epoch=audioEpoch,owner=client;renderAudio();
  try{await ensureAudio();await command('audioMap',{slot:'2',device:'browser:capture'});await command('audioRoute',{route:'microphone',enabled:'1'});const device=$('browserInput').value;const stream=await navigator.mediaDevices.getUserMedia({audio:{...(device?{deviceId:{exact:device}}:{}),channelCount:1,echoCancellation:false,noiseSuppression:false,autoGainControl:false}});if(epoch!==audioEpoch||owner!==client||socket?.readyState!==1||!state.online){stream.getTracks().forEach(track=>track.stop());throw new Error('Connection changed; arm the microphone again');}micStream=stream;micSource=audioContext.createMediaStreamSource(stream);micSource.connect(audioNode);microphone=true;audioNode.port.postMessage({kind:'gate',enabled:!!(state.transmitting&&state.dashboardPtt&&state.browserOwner===client),lease:state.micLease||0});setText($('browserStatus'),'Microphone armed. Tap PTT on to transmit, then PTT off.');browserDevices().catch(error=>notify(error.message));}
  catch(error){stopMicrophone();if(owner===client)await command('audioRoute',{route:'microphone',enabled:'0'}).catch(()=>{});throw error;}
  finally{microphoneBusy=false;renderAudio();}
}
async function browserDevices() {
  if(!navigator.mediaDevices?.enumerateDevices)throw new Error('Browser device selection is unavailable. Check HTTPS certificate trust and browser permissions; use the phone or OS controls for its speaker or headset.');const devices=await navigator.mediaDevices.enumerateDevices();
  for(const [id,kind]of [['browserInput','audioinput'],['browserOutput','audiooutput']]){const current=$(id).value;const entries=[['System default',''],...devices.filter(device=>device.kind===kind&&device.deviceId&&device.deviceId!=='default').map((device,index)=>[device.label||`${kind==='audioinput'?'Microphone':'Output'} ${index+1}`,device.deviceId])];if(current&&!entries.some(entry=>entry[1]===current))entries.push(['Saved device unavailable / allow access',current]);optionList($(id),entries,current);}
  const sink=!!(isSecureContext&&(audioContext?.setSinkId||window.AudioContext?.prototype.setSinkId));$('browserOutput').disabled=!sink;setText($('outputSupport'),sink?'Browser output selection is available.':'This browser uses the device’s system audio output. Change the speaker/headset in the phone or OS controls.');
}
function binary(buffer) {
  const header=new DataView(buffer);if(buffer.byteLength<8)return;const type=header.getUint32(0,true);
  if(type===1&&listening&&audioNode){const sequence=header.getUint32(4,true),pcm=new Int16Array(buffer,8),samples=new Float32Array(pcm.length);for(let i=0;i<pcm.length;i++)samples[i]=pcm[i]/32768;const reset=expectedAudio!==undefined&&expectedAudio!==sequence;expectedAudio=(sequence+pcm.length)>>>0;audioNode.port.postMessage({kind:'rx',samples,reset},[samples.buffer]);}
  if(type===2&&buffer.byteLength===12+2049*4){const rate=header.getUint32(4,true),generation=header.getUint32(8,true);if(generation!==scopeGeneration){clearScope();scopeGeneration=generation;}scopeRate=rate;if(!scopeFrozen){const bins=new Float32Array(buffer,12).slice();if(scopeBins)for(let i=0;i<bins.length;i++)bins[i]=10*Math.log10(.55*Math.pow(10,scopeBins[i]/10)+.45*Math.pow(10,bins[i]/10));scopeBins=bins;const raw=new Float32Array(buffer,12).slice();if(scopePeakHold){if(!scopePeaks)scopePeaks=raw.slice();else for(let i=0;i<raw.length;i++)scopePeaks[i]=Math.max(scopePeaks[i],raw[i]);}scopeRows.unshift({bins:raw,time:performance.now()});if(scopeRows.length>300)scopeRows.pop();drawScope();}}
  if(type===4&&buffer.byteLength===12+850){rfCenterHz=header.getUint32(4,true);rfSpanHz=header.getUint32(8,true);rfBins=new Uint8Array(buffer.slice(12));rfRows.unshift(rfBins.slice());if(rfRows.length>2048)rfRows.pop();drawRfScope();}
}
let scopeRate=48000,scopeGeneration=-1,scopeBins=null,scopePeaks=null,scopeRows=[],scopeFrozen=false,scopePeakHold=false;
let rfCenterHz=0,rfSpanHz=200000,rfBins=null,rfRows=[];
const palette=[[8,16,29],[24,45,116],[24,128,189],[52,213,181],[255,202,83],[255,92,75]];
function color(fraction){const scale=Math.max(0,Math.min(1,fraction))*5,index=Math.min(4,Math.floor(scale)),part=scale-index;return palette[index].map((channel,i)=>Math.round(channel+part*(palette[index+1][i]-channel)));}
function clearScope(){scopeBins=null;scopePeaks=null;scopeRows=[];drawScope();}
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
  const waterfall=prepare(bottom),image=document.createElement('canvas');image.width=512;image.height=300;const imageContext=image.getContext('2d'),pixels=imageContext.createImageData(512,300);
  for(let row=0;row<300;row++)for(let x=0;x<512;x++){const rgb=scopeRows[row]?color((column(scopeRows[row].bins,x,512)+gain-floor)/-floor):palette[0],offset=(row*512+x)*4;pixels.data[offset]=rgb[0];pixels.data[offset+1]=rgb[1];pixels.data[offset+2]=rgb[2];pixels.data[offset+3]=255;}
  imageContext.putImageData(pixels,0,0);waterfall.c.drawImage(image,waterfall.x,waterfall.y,waterfall.w,waterfall.h);
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
  const pixelRatio=devicePixelRatio||1,rows=Math.min(rfRows.length,Math.ceil(waterfall.h*pixelRatio));
  if(rows){const image=document.createElement('canvas');image.width=850;image.height=rows;const ic=image.getContext('2d'),pixels=ic.createImageData(850,rows),rfPalette=[[5,8,30],[27,25,108],[22,79,184],[0,170,216],[37,220,151],[236,231,74],[255,121,35],[255,246,220]];
    function rfColor(fraction){const position=Math.max(0,Math.min(1,fraction))*(rfPalette.length-1),index=Math.min(rfPalette.length-2,Math.floor(position)),part=position-index;return rfPalette[index].map((channel,i)=>Math.round(channel+(rfPalette[index+1][i]-channel)*part));}
    for(let row=0;row<rows;row++)for(let x=0;x<850;x++){const rgb=rfColor(rfRows[row][x]/255),offset=(row*850+x)*4;pixels.data[offset]=rgb[0];pixels.data[offset+1]=rgb[1];pixels.data[offset+2]=rgb[2];pixels.data[offset+3]=255;}
    ic.putImageData(pixels,0,0);waterfall.c.drawImage(image,waterfall.x,waterfall.y,waterfall.w,rows/pixelRatio);
  }
  const divisions=compact?2:4;c.fillStyle='#8da2b8';c.strokeStyle='#1f3040';c.lineWidth=1;
  for(let i=0;i<=divisions;i++){const px=waterfall.x+waterfall.w*i/divisions;c.beginPath();c.moveTo(px,waterfall.y);c.lineTo(px,waterfall.y+waterfall.h);c.stroke();c.textAlign=i===0?'left':i===divisions?'right':'center';const hz=center?center-span/2+span*i/divisions:0;c.fillText(center?frequency(hz):`${Math.round((i/divisions-.5)*span/1000)} kHz`,px,waterfall.y+waterfall.h+20);}
  setText($(detailId),rfRows.length?(compact?`${Math.min(rfRows.length,rows)} rows · newest at top`:`${rfRows.length} frames · newest at top; older rows scroll off the bottom`):(compact?'Waiting for RF frames':'No RF frames received'));
}
function drawRfScope(){
  drawMobileRfPreview();if(!$('rfScope')||$('rfScope').hidden)return;
  drawRfSpectrumCanvas('rfSpectrumCanvas','rfSpectrumDetail');
  drawRfWaterfallCanvas('rfWaterfallCanvas','rfWaterfallDetail');
}
function tuneRfFromCanvas(event,id){
  if(!state?.online||state.busy||!rfBins)return;
  const canvas=$(id),rect=canvas.getBoundingClientRect(),plotLeft=54,plotWidth=Math.max(1,rect.width-70);
  const fraction=Math.max(0,Math.min(1,(event.clientX-rect.left-plotLeft)/plotWidth));
  const activeVfo=state.activeVfo==='B'?1:0,center=rfCenterHz||state.rfScope?.centerHz||state.vfos[activeVfo]?.frequency||0;
  const target=center-rfSpanHz/2+fraction*rfSpanHz;
  if(!Number.isFinite(target)||target<30000||target>60000000)throw new Error('This spectrum position is outside the FT-710 tuning range');
  $('controlVfo').value=state.activeVfo;dirty.delete('frequency');
  return command('frequency',{vfo:state.activeVfo,hz:String(Math.round(target))},'frequency');
}
async function exportCsv(){if(!state)return;const cell=value=>'"'+String(value??'').replace(/"/g,'""')+'"',utc=new Date().toISOString();const rows=[['Name','Group','Hamlib ID','Value','Raw value','Access','Range','Status','VFO','UTC']];state.vfos.forEach((vfo,index)=>{const name=index?'B':'A';for(const [field,label,valid,reading]of [['frequency','frequency (Hz)',vfo.frequencyValid,String(vfo.frequency)],['mode','mode / width',vfo.modeValid,vfo.mode+' / '+vfo.width]])rows.push(['VFO '+name+' '+label,'Radio / VFO','VFO'+name+'.'+field,valid?reading:'',valid?reading:'','Read / write','',valid?'Live':'Unavailable',name,utc]);});for(const item of state.settings)rows.push([item.name,item.group,item.id,item.valid?item.display:'',item.valid?item.value:'',item.writable?'Read / write':'Read only',item.range,item.status,state.settingsVfo,utc]);const blob=new Blob(['\ufeff'+rows.map(row=>row.map(cell).join(',')).join('\r\n')],{type:'text/csv;charset=utf-8'}),url=URL.createObjectURL(blob),link=element('a');link.href=url;link.download='FT710-settings.csv';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
function setup(){
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
  $('loginForm').addEventListener('submit',protect(async event=>{event.preventDefault();const reply=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({code:$('accessCode').value})});const data=await reply.json();if(!reply.ok){$('loginError').textContent=data.error;return;}$('accessCode').value='';closing=false;connectWeb();}));
  $('connectionForm').addEventListener('submit',protect(event=>{event.preventDefault();clearTimeout(preferenceSaveTimer);if(state.portOpen)return command('disconnect');const fields={kind:$('connectionKind').value};for(const key of ['endpoint','baud','stopBits','flow','poll'])fields[key]=$(key).value;for(const key of ['connectionKind','endpoint','baud','stopBits','flow','poll'])dirty.delete(key);return command('connect',fields);}));
  $('connectionKind').addEventListener('change',()=>{if($('connectionKind').value==='1'&&!$('endpoint').value.includes(':'))$('endpoint').value='127.0.0.1:4532';if($('connectionKind').value==='0'&&$('endpoint').value.includes(':'))$('endpoint').value='COM3';});
  $('saveSharing').addEventListener('click',protect(async()=>{await command('sharing',{enabled:$('shareEnabled').checked?'1':'0',port:$('sharePort').value});dirty.delete('shareEnabled');dirty.delete('sharePort');}));
  $('ptt').addEventListener('click',protect(()=>command('ptt',{enabled:state.dashboardPtt||state.pttPending?'0':'1'})));
  $('frequencyForm').addEventListener('submit',protect(event=>{event.preventDefault();return command('frequency',{vfo:$('controlVfo').value,hz:String(parseFrequencyInput($('frequency').value))},'frequency');}));
  $('mode').addEventListener('change',protect(async()=>{dirty.delete('width');edits.set('width',{revision:null,until:Date.now()+12000});try{const ack=await command('mode',{vfo:$('controlVfo').value,mode:$('mode').value,width:'0'},'mode');const edit=edits.get('width');if(edit)edit.revision=ack.revision||0;}catch(error){edits.delete('width');throw error;}}));
  $('filterForm').addEventListener('submit',protect(event=>{event.preventDefault();return command('mode',{vfo:$('controlVfo').value,mode:$('mode').value,width:$('width').value},'width');}));
  $('controlVfo').addEventListener('change',protect(()=>{dirty.delete('frequency');dirty.delete('width');return command('vfo',{vfo:$('controlVfo').value},'controlVfo');}));
  const tune=(steps,target=$('controlVfo').value)=>{if(!state?.online||state.busy||!steps)return;dirty.delete('frequency');return command('step',{vfo:target,hz:String(steps*Number($('step').value))},'frequency');};
  $('up').addEventListener('click',protect(()=>tune(1)));$('down').addEventListener('click',protect(()=>tune(-1)));
  let drag,lastX,motion=0,tuneTimer;const steps=new Map(),queue=(delta,target=$('controlVfo').value)=>{steps.set(target,Math.max(-10,Math.min(10,(steps.get(target)||0)+delta)));clearTimeout(tuneTimer);tuneTimer=setTimeout(protect(async()=>{const batches=[...steps];steps.clear();for(const [vfo,count]of batches)await tune(count,vfo);}),90);};
  $('dial').addEventListener('pointerdown',event=>{if(!state?.online)return;drag=event.pointerId;lastX=event.clientX;motion=0;$('dial').setPointerCapture(drag);});$('dial').addEventListener('pointermove',event=>{if(drag!==event.pointerId)return;motion+=event.clientX-lastX;lastX=event.clientX;const count=Math.trunc(motion/12);if(count){motion-=count*12;queue(count);}});for(const name of ['pointerup','pointercancel','lostpointercapture'])$('dial').addEventListener(name,()=>drag=undefined);
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
  const savedVolume=localStorage.getItem('browserVolume');if(savedVolume!==null&&Number.isFinite(Number(savedVolume)))$('browserVolume').value=Math.max(0,Math.min(100,Number(savedVolume)));
  $('browserInput').addEventListener('change',()=>{localStorage.setItem('browserInput',$('browserInput').value);if(microphone||microphoneBusy){stopMicrophone();notify('Microphone selection changed. Arm the browser microphone again.',false);if(state?.browserOwner===client)command('ptt',{enabled:'0'}).catch(()=>{});}});
  $('browserOutput').addEventListener('change',protect(async()=>{await ensureAudio();if(!audioContext.setSinkId)throw new Error('This browser uses the system output');await audioContext.setSinkId($('browserOutput').value);localStorage.setItem('browserOutput',$('browserOutput').value);}));$('browserVolume').addEventListener('input',()=>{localStorage.setItem('browserVolume',$('browserVolume').value);if(listeningGain)listeningGain.gain.value=Number($('browserVolume').value)/100;});
  for(const id of ['scopeSpan','scopeGain','scopeFloor']){const saved=localStorage.getItem(id);if([...$(id).options].some(option=>option.value===saved))$(id).value=saved;$(id).addEventListener('change',()=>{localStorage.setItem(id,$(id).value);drawScope();});}
  for(const id of ['rfSpan','rfCat2Port','rfCat2Baud']){let saved=localStorage.getItem(id);if(id==='rfCat2Baud'&&saved==='38400'){saved='4800';localStorage.setItem(id,saved);}if(saved&&([...$(id).options].some(option=>option.value===saved)||id==='rfCat2Port'))$(id).value=saved;$(id).addEventListener('change',()=>{localStorage.setItem(id,$(id).value);if(id==='rfSpan')dirty.delete(id);drawRfScope();});}
  const savedRfLevel=localStorage.getItem('rfWaterfallLevel');if(savedRfLevel!==null&&Number.isFinite(Number(savedRfLevel))&&Number(savedRfLevel)>=-30&&Number(savedRfLevel)<=30&&Number.isInteger(Number(savedRfLevel)*2))$('rfWaterfallLevel').value=Number(savedRfLevel).toFixed(1);
  $('rfWaterfallLevel').addEventListener('change',()=>localStorage.setItem('rfWaterfallLevel',Number($('rfWaterfallLevel').value).toFixed(1)));
  for(const id of ['rfCapture','mobileRfCapture'])$(id).addEventListener('click',protect(toggleRfCapture));
  $('rfApplySpan').addEventListener('click',protect(async()=>{const port=$('rfCat2Port').value.trim(),level=Number($('rfWaterfallLevel').value);if(!/^COM\d+$/i.test(port))throw new Error('Enter the radio Standard / CAT-2 COM port, for example COM5');if(!Number.isFinite(level)||level < -30||level > 30||!Number.isInteger(level*2))throw new Error('Waterfall level must be -30 to +30 dB in 0.5 dB steps');await command('rfSpan',{port,baud:$('rfCat2Baud').value,span:$('rfSpan').value,level:level.toFixed(1)});dirty.delete('rfSpan');dirty.delete('rfWaterfallLevel');localStorage.setItem('rfWaterfallLevel',level.toFixed(1));notify('Radio confirmed CENTER mode, RF span, and waterfall level.',false);}));
  $('rfClear').addEventListener('click',()=>{rfRows=[];rfBins=null;drawRfScope();});
  for(const id of ['rfSpectrumCanvas','rfWaterfallCanvas','mobileRfSpectrumCanvas','mobileRfWaterfallCanvas'])$(id).addEventListener('click',protect(event=>tuneRfFromCanvas(event,id)));
  const savedStep=localStorage.getItem('step');if([...$('step').options].some(option=>option.value===savedStep))$('step').value=savedStep;$('step').addEventListener('change',()=>localStorage.setItem('step',$('step').value));
  $('scopeFreeze').addEventListener('click',()=>{scopeFrozen=!scopeFrozen;setText($('scopeFreeze'),scopeFrozen?'Resume':'Freeze');$('scopeFreeze').classList.toggle('selected',scopeFrozen);drawScope();});$('scopePeak').addEventListener('click',()=>{scopePeakHold=!scopePeakHold;scopePeaks=null;setText($('scopePeak'),'Peak hold: '+(scopePeakHold?'On':'Off'));$('scopePeak').classList.toggle('selected',scopePeakHold);drawScope();});$('scopeClear').addEventListener('click',clearScope);
  $('logout').addEventListener('click',protect(async()=>{if(state?.browserOwner===client)await command('ptt',{enabled:'0'});closing=true;clearTimeout(reconnectTimer);socket?.close();stopClientAudio();await fetch('/api/logout',{method:'POST'});$('app').hidden=true;$('mobilePttBar').hidden=true;$('login').hidden=false;}));
  window.addEventListener('resize',()=>{drawScope();drawRfScope();});window.addEventListener('pagehide',()=>{closing=true;stopClientAudio();socket?.close();});
  setInterval(()=>setText($('clock'),new Date().toISOString().slice(11,19)+' UTC'),1000);
  const code=new URLSearchParams(location.hash.slice(1)).get('code');if(code){$('accessCode').value=code;history.replaceState(null,'',location.pathname);}
  fetch('/api/state').then(reply=>{if(reply.ok){closing=false;connectWeb();}}).catch(()=>{});
  const sink=!!(isSecureContext&&window.AudioContext?.prototype.setSinkId);$('browserOutput').disabled=!sink;$('browserInput').disabled=$('browserDevices').disabled=!navigator.mediaDevices?.enumerateDevices;
  setText($('outputSupport'),sink?'Browser output selection is available.':'This browser uses the device’s system audio output.');
  if(navigator.mediaDevices?.enumerateDevices)browserDevices().catch(()=>{});
  const support=element('p',browserMicAvailable()?'Tap Arm browser microphone and allow microphone access. On iPhone, select the speaker or headset using the phone’s audio controls.':'Microphone capture needs a supported browser and trusted HTTPS. Install and trust the station certificate before opening the dashboard.','muted');$('outputSupport').after(support);$('microphone').disabled=!browserMicAvailable();if(!browserMicAvailable())setText($('browserMappings'),'Use browser receive mapping');
}
setup();
