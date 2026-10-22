'use strict';
// Browser-independent controllers are also exercised by the offline checks.
class PreferenceSaver {
  constructor(read, send, settled, status, delay=350) {
    Object.assign(this,{read,send,settled,status,delay});this.versions=new Map();this.revision=0;this.busy=false;
  }
  edit(key) { this.versions.set(key,++this.revision);this.status('Unsaved changes');this.schedule(); }
  schedule(delay=this.delay) { clearTimeout(this.timer);this.timer=setTimeout(()=>this.flush(),delay); }
  async flush() {
    if(this.busy||!this.versions.size)return;
    const versions=new Map(this.versions);this.busy=true;this.status('Saving…');
    try {
      const snapshot=this.read();
      await this.send({...snapshot,changed:[...versions.keys()].join(',')});
      for(const [key,revision]of versions)if(this.versions.get(key)===revision){this.versions.delete(key);this.settled(key);}
      this.status(this.versions.size?'Unsaved changes':'Saved');
      if(this.versions.size)this.schedule(0);
    } catch(error) { this.status('Save failed: '+error.message,true); }
    finally { this.busy=false; }
  }
}
class TuningQueue {
  constructor(send,delay=90,onError=()=>{}) { this.send=send;this.delay=delay;this.onError=onError;this.steps=new Map();this.busy=false;this.epoch=0; }
  add(vfo,hz) { if(!Number.isFinite(hz))return;this.steps.set(vfo,(this.steps.get(vfo)||0)+hz);if(!this.timer)this.timer=setTimeout(()=>{this.timer=null;this.flush().catch(this.onError);},this.delay); }
  clear() { ++this.epoch;clearTimeout(this.timer);this.timer=null;this.steps.clear(); }
  async flush() {
    if(this.busy)return;
    clearTimeout(this.timer);this.timer=null;this.busy=true;const epoch=this.epoch;
    try {
      const batches=[...this.steps];this.steps.clear();
      for(const [vfo,total]of batches){let remaining=total;while(remaining&&epoch===this.epoch){const delta=Math.max(-1000000,Math.min(1000000,remaining));await this.send(vfo,delta);remaining-=delta;}}
    } finally { this.busy=false;if(this.steps.size)this.timer=setTimeout(()=>{this.timer=null;this.flush().catch(this.onError);},this.delay); }
  }
}
class RfHistory {
  constructor(limit=2048) { this.limit=limit;this.clear(); }
  clear() { this.rows=[];this.center=0;this.span=0;this.generation=-1;this.sequence=-1;this.received=0; }
  push(center,span,generation,sequence,bins) {
    // Keep older rows in their original screen positions as the VFO moves.
    if(this.span!==span||this.generation!==generation)this.clear();
    if(this.sequence===sequence)return false;
    Object.assign(this,{center,span,generation,sequence,received:performance.now()});
    this.rows.unshift(bins);if(this.rows.length>this.limit)this.rows.pop();return true;
  }
  fresh() { return this.rows.length>0&&performance.now()-this.received<1000; }
}
function mergeState(previous,patch){
  const merge=(before,after)=>{const result={...before};for(const [key,value]of Object.entries(after)){if(value&&typeof value==='object'&&!Array.isArray(value))result[key]=merge(before?.[key]||{},value);else result[key]=value;}return result;};
  const next=merge(previous,patch.fields),updates=new Map(patch.settings.map(item=>[item.id,item]));
  next.settings=next.settings.map(item=>updates.has(item.id)?{...item,...updates.get(item.id)}:item);
  next.activity=previous.activity.slice(patch.activity.remove).concat(patch.activity.append);next.type='state';return next;
}
if(typeof module!=='undefined')module.exports={PreferenceSaver,TuningQueue,RfHistory,mergeState};
