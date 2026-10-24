/* PCM playback over the existing authenticated Home Assistant WebSocket. */
class VoicePEMonitorCard extends HTMLElement {
  setConfig(config) {
    if (!config.node_id) throw new Error('Укажите node_id');
    this.stop();
    this.config = config;
    if (!this.shadowRoot) this.attachShadow({mode: 'open'});
    this.shadowRoot.innerHTML = `<ha-card><div class="body"><h3></h3><p role="status">Прослушивание выключено</p><button class="listen">Слушать</button><button class="uplink" hidden>Радионяня</button></div></ha-card><style>.body{padding:20px}button{padding:10px;margin:4px;border-radius:8px;cursor:pointer}h3{margin-top:0}</style>`;
    this.shadowRoot.querySelector('h3').textContent = config.title || 'Радионяня Voice PE';
    this.shadowRoot.querySelector('.listen').onclick = () => this.context ? this.stop() : this.start();
    const uplink = this.shadowRoot.querySelector('.uplink');
    uplink.hidden = !config.switch_entity;
    uplink.onclick = () => {
      const enabled = this._hass.states[config.switch_entity]?.state === 'on';
      this._hass.callService('switch', enabled ? 'turn_off' : 'turn_on', {entity_id: config.switch_entity})
        .catch(() => this.status('Не удалось переключить режим'));
    };
  }
  set hass(hass) {
    this._hass = hass;
    if (this.config?.switch_entity && this.shadowRoot) {
      const on = hass.states[this.config.switch_entity]?.state === 'on';
      this.shadowRoot.querySelector('.uplink').textContent = on ? 'Выключить передачу' : 'Включить передачу';
    }
  }
  status(text) { if (this.shadowRoot) this.shadowRoot.querySelector('p').textContent = text; }
  async start() {
    const Context = window.AudioContext || window.webkitAudioContext;
    if (!Context) { this.status('Браузер не поддерживает Web Audio'); return; }
    const generation = this.generation = (this.generation || 0) + 1;
    try {
      const context = this.context = new Context();
      await context.resume();
      if (generation !== this.generation) return;
      this.nextTime = 0;
      this.shadowRoot.querySelector('.listen').textContent = 'Остановить';
      const unsubscribe = await this._hass.connection.subscribeMessage(event => {
        if (generation !== this.generation || !this.context) return;
        if (event.type === 'status') {
          this.status(event.connected ? 'Передача включена • слушаем' : 'Нет потока — включите «Радионяня» на колонке');
          if (!event.connected) this.clearAudio();
          return;
        }
        if (event.type !== 'audio' || event.rate !== 16000) return;
        const bytes = Uint8Array.from(atob(event.pcm), c => c.charCodeAt(0));
        const view = new DataView(bytes.buffer);
        const buffer = this.context.createBuffer(1, bytes.length / 2, event.rate);
        const channel = buffer.getChannelData(0);
        for (let i = 0; i < channel.length; i++) channel[i] = view.getInt16(i * 2, true) / 32768;
        const now = this.context.currentTime;
        if (this.nextTime > now + 0.3) this.clearAudio();
        this.nextTime = Math.max(this.nextTime || 0, now + 0.06);
        const source = this.context.createBufferSource();
        source.buffer = buffer; source.connect(this.context.destination);
        this.sources ||= new Set(); this.sources.add(source);
        source.onended = () => this.sources.delete(source);
        source.start(this.nextTime); this.nextTime += buffer.duration;
      }, {type: 'voice_pe_monitor/listen', node_id: this.config.node_id});
      if (generation !== this.generation) unsubscribe();
      else this.unsubscribe = unsubscribe;
    } catch (error) { if (generation === this.generation) { this.stop(); this.status('Не удалось подключиться: ' + error.message); } }
  }
  clearAudio() {
    for (const source of this.sources || []) { try { source.stop(); } catch (_) {} }
    this.sources?.clear(); this.nextTime = 0;
  }
  stop() {
    this.generation = (this.generation || 0) + 1;
    this.unsubscribe?.(); this.unsubscribe = null;
    this.clearAudio();
    this.context?.close(); this.context = null;
    if (this.shadowRoot) this.shadowRoot.querySelector('.listen').textContent = 'Слушать';
    this.status('Прослушивание выключено');
  }
  disconnectedCallback() { this.stop(); }
  getCardSize() { return 3; }
}
customElements.define('voice-pe-monitor-card', VoicePEMonitorCard);
