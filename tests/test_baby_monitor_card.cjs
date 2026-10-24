// Exercise the actual card's Web Audio conversion and stop behavior without a bundler.
const {test} = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

test('authenticated subscription, signed little-endian PCM, stop and late events', async () => {
  let Card, event, unsubscribed = 0;
  const buffers = [], sources = [];
  class AudioContext {
    currentTime = 1;
    destination = {};
    async resume() {}
    async close() { this.closed = true; }
    createBuffer(channels, length, rate) {
      const data = new Float32Array(length); buffers.push(data);
      return {getChannelData: () => data, duration: length/rate};
    }
    createBufferSource() {
      const source = {connect() {}, start(time) {this.time=time;}, stop() {this.stopped=true;}};
      sources.push(source); return source;
    }
  }
  class HTMLElement {
    attachShadow() {
      const nodes = new Map();
      this.shadowRoot = {querySelector(selector) {if (!nodes.has(selector)) nodes.set(selector, {});return nodes.get(selector);}};
    }
  }
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../custom_components/voice_pe_monitor/www/monitor-card.js'), 'utf8'), {
    HTMLElement, window: {AudioContext}, customElements: {define(name, cls) {Card=cls;}},
    Uint8Array, DataView, atob: x => Buffer.from(x, 'base64').toString('binary'),
  });
  const card = new Card();
  card.setConfig({node_id:'voice'});
  card.hass = {connection: {async subscribeMessage(callback, command) {
    assert.equal(command.type,'voice_pe_monitor/listen'); assert.equal(command.node_id,'voice');
    event=callback;return () => {unsubscribed++;};
  }}};
  await card.start();
  event({type:'audio',rate:16000,pcm:Buffer.from([1,0,255,127,0,128,255,255]).toString('base64')});
  assert.deepEqual(Array.from(buffers[0]),[1/32768,32767/32768,-1,-1/32768]);
  assert.ok(sources[0].time>=1.06);
  event({type:'status',connected:false});
  assert.equal(sources[0].stopped,true);
  card.stop(); assert.equal(unsubscribed,1);
  event({type:'audio',rate:16000,pcm:'AAAA'});
  assert.equal(buffers.length,1);
  assert.equal(card.context,null);
});
