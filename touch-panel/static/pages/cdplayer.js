/* Page 1: CD Player View with Real Progress Bar, Track Info & PC Audio Toggle */
const PageCDPlayer = {
  name: 'CD Player',
  iconSVG: `<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2"/><circle cx="12" cy="12" r="3" fill="none" stroke="currentColor" stroke-width="2"/></svg>`,
  interval: null,
  currentState: 'idle',
  currentTrack: 1,

  render(container) {
    container.innerHTML = `
      <div class="cd-player-container">
        <div class="cd-disc-art" id="cd-disc" style="cursor: pointer;" title="Doble clic o doble toque para alternar Proxmox por Jellyfin">
          <svg viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 14.5c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.66 0-3 1.34-3 3s1.34 3 3 3 3-1.34 3-3-1.34-3-3-3z"/></svg>
        </div>
        
        <div style="text-align: center; max-width: 90%;">
          <div id="cd-album" style="font-size: 0.85rem; font-weight: 700; color: var(--ctp-lavender); text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 2px;">Álbum: Desconocido</div>
          <h2 id="cd-title" style="font-size: 1.25rem; font-weight: 700; color: var(--ctp-text);">Cargando CD...</h2>
          <p id="cd-artist" style="font-size: 0.95rem; color: var(--ctp-subtext0); margin-top: 2px;">--</p>
          <div style="margin-top: 6px;">
            <span id="cd-badge" class="badge idle">IDLE</span>
          </div>
        </div>

        <div style="width: 85%; max-width: 500px; display: flex; flex-direction: column; align-items: center; gap: 4px;">
          <div class="progress-bar-container">
            <div class="progress-bar-fill" id="cd-progress"></div>
          </div>
          <div id="cd-time" style="font-size: 0.8rem; color: var(--ctp-overlay1); font-weight: 600; font-family: monospace;">00:00 / 00:00</div>
        </div>

        <div class="cd-controls">
          <button class="btn" id="btn-pc-audio" title="Audio de PC (WiFi UDP 24-bit/48kHz)">
            <svg viewBox="0 0 24 24"><path d="M12 3C6.95 3 2.69 5.86.6 10.02a1 1 0 00.38 1.34l1.62.97a1 1 0 001.34-.38C5.46 8.35 8.5 6 12 6s6.54 2.35 8.06 5.95a1 1 0 001.34.38l1.62-.97a1 1 0 00.38-1.34C21.31 5.86 17.05 3 12 3zm0 6c-3.15 0-5.87 1.83-7.14 4.54a1 1 0 00.35 1.25l1.64.98a1 1 0 001.27-.3c.77-1.46 2.24-2.47 3.88-2.47s3.11 1.01 3.88 2.47a1 1 0 001.27.3l1.64-.98a1 1 0 00.35-1.25C17.87 10.83 15.15 9 12 9zm0 6a2.5 2.5 0 100 5 2.5 2.5 0 000-5z"/></svg>
          </button>
          <button class="btn" id="btn-prev" title="Anterior">
            <svg viewBox="0 0 24 24"><path d="M6 6h2v12H6zm3.5 6l8.5 6V6z"/></svg>
          </button>
          <button class="btn btn-primary" id="btn-play" title="Reproducir/Pausa">
            <svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>
          </button>
          <button class="btn" id="btn-next" title="Siguiente">
            <svg viewBox="0 0 24 24"><path d="M6 18l8.5-6L6 6v12zM16 6v12h2V6h-2z"/></svg>
          </button>
          <button class="btn" id="btn-eject" title="Expulsar">
            <svg viewBox="0 0 24 24"><path d="M12 5L5 15h14l-7-10zm-7 12h14v2H5v-2z"/></svg>
          </button>
        </div>
      </div>
    `;

    this.bindEvents(container);
    this.updateStatus();
    this.startPolling();
  },

  bindEvents(container) {
    container.querySelector('#btn-pc-audio').onclick = () => this.sendAction('pc_audio_toggle');
    container.querySelector('#btn-play').onclick = () => this.handlePlayPause();
    container.querySelector('#btn-prev').onclick = () => this.sendAction('prev');
    container.querySelector('#btn-next').onclick = () => this.sendAction('next');
    container.querySelector('#btn-eject').onclick = () => this.sendAction('eject');
  },

  formatTime(seconds) {
    const s = Math.max(0, Math.floor(seconds || 0));
    const mins = Math.floor(s / 60);
    const secs = s % 60;
    return `${mins.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
  },

  async handlePlayPause() {
    let action = 'play';
    if (this.currentState === 'playing') {
      action = 'pause';
    } else if (this.currentState === 'paused') {
      action = 'resume';
    } else {
      action = 'play';
    }
    await this.sendAction(action);
  },

  async sendAction(action, payload = null) {
    try {
      await fetch('/api/cdplayer/action', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action, payload })
      });
      setTimeout(() => this.updateStatus(), 200);
    } catch (err) {
      console.error('Action error:', err);
    }
  },

  async updateStatus() {
    try {
      const disc = document.getElementById('cd-disc');
      const album = document.getElementById('cd-album');
      const title = document.getElementById('cd-title');
      const artist = document.getElementById('cd-artist');
      const badge = document.getElementById('cd-badge');
      const progress = document.getElementById('cd-progress');
      const timeTxt = document.getElementById('cd-time');
      const playBtn = document.getElementById('btn-play');
      const pcAudioBtn = document.getElementById('btn-pc-audio');

      if (!disc) return;

      // STANDARD CD PLAYER / PC AUDIO METADATA SYNC
      const res = await fetch('/api/cdplayer?endpoint=status');
      const data = await res.json();

      if (data.error) {
        album.textContent = '';
        title.textContent = 'Sin conexión CD Player';
        artist.textContent = '--';
        badge.textContent = 'ERROR';
        badge.className = 'badge error';
        disc.classList.remove('playing');
        timeTxt.textContent = '00:00 / 00:00';
        progress.style.width = '0%';
        if (pcAudioBtn) pcAudioBtn.classList.remove('btn-primary');
        return;
      }

      this.currentState = data.state || 'idle';
      this.currentTrack = data.current_track || 1;

      if (data.pc_audio_active || this.currentState === 'pc_audio') {
        if (pcAudioBtn) {
          pcAudioBtn.classList.add('btn-primary');
          pcAudioBtn.style.backgroundColor = 'var(--ctp-green)';
          pcAudioBtn.style.borderColor = 'var(--ctp-green)';
        }
        badge.textContent = 'PC AUDIO';
        badge.className = 'badge online';
        album.textContent = 'MODO AUDIO DE PC';
        title.textContent = 'Receptor UDP 24-bit / 48kHz';
        artist.textContent = 'Transmitiendo desde tu PC';
        progress.style.width = '100%';
        timeTxt.textContent = 'STREAMING ONLINE';
        return;
      } else {
        if (pcAudioBtn) {
          pcAudioBtn.classList.remove('btn-primary');
          pcAudioBtn.style.backgroundColor = '';
          pcAudioBtn.style.borderColor = '';
        }
      }

      if (this.currentState === 'no_disc') {
        badge.textContent = 'SIN DISCO';
        badge.className = 'badge idle';
        album.textContent = 'BANDEJA VACÍA';
        title.textContent = 'Inserta un CD Audio';
        artist.textContent = 'Listo para reproducir';
        disc.classList.remove('playing');
        playBtn.innerHTML = `<svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>`;
        progress.style.width = '0%';
        timeTxt.textContent = '00:00 / 00:00';
        return;
      }

      if (this.currentState === 'starting') {
        badge.textContent = 'CARGANDO';
        badge.className = 'badge starting';
        disc.classList.remove('playing');
        playBtn.innerHTML = `<svg viewBox="0 0 24 24"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>`;
      } else if (this.currentState === 'playing') {
        badge.textContent = 'PLAYING';
        badge.className = 'badge playing';
        disc.classList.add('playing');
        playBtn.innerHTML = `<svg viewBox="0 0 24 24"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>`;
      } else {
        badge.textContent = this.currentState.toUpperCase();
        badge.className = `badge ${this.currentState}`;
        disc.classList.remove('playing');
        playBtn.innerHTML = `<svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>`;
      }

      badge.style.backgroundColor = '';
      badge.style.color = '';
      badge.style.borderColor = '';

      album.textContent = data.album_title ? `Álbum: ${data.album_title}` : 'Disco CD Audio';

      let trackObj = null;
      if (data.tracks && data.tracks.length >= this.currentTrack) {
        trackObj = data.tracks[this.currentTrack - 1];
      }

      const trackTitle = trackObj && trackObj.title ? trackObj.title : `Pista ${this.currentTrack}`;
      title.textContent = `${this.currentTrack}. ${trackTitle}`;
      artist.textContent = data.album_artist || (trackObj && trackObj.artist ? trackObj.artist : 'Reproductor CD Hi-Fi');

      const elapsed = data.elapsed_time || 0;
      const duration = trackObj && trackObj.duration ? trackObj.duration : 0;

      if (duration > 0 && this.currentState === 'playing') {
        const pct = Math.min(100, Math.max(0, (elapsed / duration) * 100));
        progress.style.width = `${pct}%`;
        timeTxt.textContent = `${this.formatTime(elapsed)} / ${this.formatTime(duration)}`;
      } else if (duration > 0) {
        progress.style.width = '0%';
        timeTxt.textContent = `00:00 / ${this.formatTime(duration)}`;
      } else {
        progress.style.width = '0%';
        timeTxt.textContent = `${this.formatTime(elapsed)} / --:--`;
      }
    } catch (err) {
      console.error('Update status error:', err);
    }
  },

  startPolling() {
    this.stopPolling();
    this.interval = setInterval(() => this.updateStatus(), 1000);
  },

  stopPolling() {
    if (this.interval) clearInterval(this.interval);
  }
};
