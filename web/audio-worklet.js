class StationAudio extends AudioWorkletProcessor {
  constructor() {
    super(); this.rx = new Float32Array(32768); this.read = 0; this.write = 0; this.phase = 0; this.buffering = true;
    this.listen = false; this.gate = false; this.lease = 0; this.micPosition = 0; this.micNext = 0; this.previous = 0; this.havePrevious = false;
    this.mic = new Int16Array(960); this.micCount = 0; this.peak = 0; this.peakFrames = 0;
    this.port.onmessage = ({data}) => {
      if (data.kind === 'rx') {
        if (this.write - this.read + data.samples.length >= 32768 || data.reset) this.resetRx();
        for (const sample of data.samples) this.rx[this.write++ & 32767] = sample;
        if (this.write - this.read > 7200) { this.read = this.write - 2880; this.phase = 0; }
      } else if (data.kind === 'listen') { this.listen = data.enabled; if (!this.listen) this.resetRx(); }
      else if (data.kind === 'reset') this.resetRx();
      else if (data.kind === 'gate') {
        if (this.gate !== data.enabled || this.lease !== data.lease) { this.micCount = 0; this.havePrevious = false; this.micPosition = this.micNext = 0; }
        this.gate = data.enabled; this.lease = data.lease;
      }
    };
  }
  resetRx() { this.read = this.write; this.phase = 0; this.buffering = true; }
  micSample(sample) {
    this.mic[this.micCount++] = Math.round(Math.max(-1, Math.min(1, sample)) * 32767);
    if (this.micCount === 960) {
      const buffer = new ArrayBuffer(8 + 1920), header = new DataView(buffer);
      header.setUint32(0, 3, true); header.setUint32(4, this.lease, true); new Int16Array(buffer, 8).set(this.mic);
      this.port.postMessage({kind:'mic', buffer}, [buffer]); this.micCount = 0;
    }
  }
  process(inputs, outputs) {
    const input = inputs[0]?.[0], output = outputs[0]?.[0]; if (!output) return true;
    const rxStep = 48000 / sampleRate, micStep = sampleRate / 48000;
    for (let i = 0; i < output.length; i++) {
      const sample = input ? input[i] || 0 : 0; this.peak = Math.max(this.peak, Math.abs(sample));
      if (this.gate && input) {
        if (!this.havePrevious) { this.micSample(sample); this.previous = sample; this.havePrevious = true; this.micNext = micStep; this.micPosition = 1; }
        else {
          while (this.micNext <= this.micPosition) { this.micSample(this.previous + (sample - this.previous) * (this.micNext - (this.micPosition - 1))); this.micNext += micStep; }
          this.previous = sample; this.micPosition++;
        }
      }
      if (this.buffering && this.write - this.read >= 2880) this.buffering = false;
      let received = 0;
      if (this.listen && !this.buffering && this.write - this.read >= Math.ceil(rxStep) + 1) {
        const a = this.rx[this.read & 32767], b = this.rx[(this.read + 1) & 32767]; received = a + (b - a) * this.phase;
        this.phase += rxStep; const advance = Math.floor(this.phase); this.read += advance; this.phase -= advance;
      } else if (this.listen && !this.buffering) this.buffering = true;
      output[i] = received;
    }
    for (let channel = 1; channel < outputs[0].length; channel++) outputs[0][channel].set(output);
    this.peakFrames += output.length;
    if (this.peakFrames >= sampleRate / 10) { this.port.postMessage({kind:'peak',value:this.peak}); this.peak = this.peakFrames = 0; }
    return true;
  }
}
registerProcessor('station-audio', StationAudio);
