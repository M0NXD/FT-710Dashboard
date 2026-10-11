'use strict';
// Receive playback fallback for browsers without AudioWorklet support.
// AudioBufferSourceNode resamples the 48 kHz wire audio to the device rate.
class StationPlayback {
  constructor(context) {
    this.context=context;this.output=context.createGain();this.sources=new Set();
    this.listening=false;this.nextTime=0;this.supportsMicrophone=false;
    this.port={postMessage:data=>this.message(data)};
  }
  connect(destination) { this.output.connect(destination);return destination; }
  reset() {
    for(const source of this.sources){try{source.stop();}catch{}source.disconnect();}
    this.sources.clear();this.nextTime=0;
  }
  message(data) {
    if(data.kind==='listen'){this.listening=!!data.enabled;if(!this.listening)this.reset();return;}
    if(data.kind==='reset'){this.reset();return;}
    if(data.kind!=='rx'||!this.listening)return;
    const now=this.context.currentTime;
    if(data.reset||this.nextTime-now>.15)this.reset();
    if(this.nextTime<now)this.nextTime=now+.06;
    const buffer=this.context.createBuffer(1,data.samples.length,48000);
    buffer.copyToChannel(data.samples,0);
    const source=this.context.createBufferSource();source.buffer=buffer;source.connect(this.output);
    this.sources.add(source);source.onended=()=>{this.sources.delete(source);source.disconnect();};
    source.start(this.nextTime);this.nextTime+=buffer.duration;
  }
}
