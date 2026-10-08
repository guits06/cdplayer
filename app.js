// API configuration - uses local relative paths
const API_BASE = "";

// State variables
let currentStatus = {
    state: "idle",
    current_track: 1,
    tracks: [],
    dac_name: "default",
    elapsed_time: 0.0,
    error_message: ""
};

let pollInterval = null;

// DOM Elements
const dacSelect = document.getElementById("dac-select");
const cdDisc = document.getElementById("cd-disc");
const cdStatusText = document.getElementById("cd-status-text");
const currentTrackTitle = document.getElementById("current-track-title");
const currentTrackSubtitle = document.getElementById("current-track-subtitle");
const timeElapsed = document.getElementById("time-elapsed");
const trackTotalTime = document.getElementById("track-total-time");
const progressBarBg = document.getElementById("progress-bar-bg");
const progressBarFill = document.getElementById("progress-bar-fill");
const tracklistContainer = document.getElementById("tracklist-container");

const btnPrev = document.getElementById("btn-prev");
const btnPlayPause = document.getElementById("btn-play-pause");
const btnStop = document.getElementById("btn-stop");
const btnNext = document.getElementById("btn-next");
const btnLoad = document.getElementById("btn-load");
const btnEject = document.getElementById("btn-eject");
const btnRefresh = document.getElementById("btn-refresh");

const playIcon = document.getElementById("play-icon");
const pauseIcon = document.getElementById("pause-icon");
const btnPCAudio = document.getElementById("btn-pc-audio");
const btnBluetooth = document.getElementById("btn-bluetooth");
const bufferSelect = document.getElementById("buffer-select");



const modulesListEl = document.getElementById("modules-list");

// Utility: format seconds to mm:ss
function formatTime(secs) {
    if (isNaN(secs) || secs < 0) return "00:00";
    const minutes = Math.floor(secs / 60);
    const seconds = Math.floor(secs % 60);
    return `${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`;
}

// Fetch status from API
async function fetchStatus() {
    try {
        const response = await fetch(`${API_BASE}/api/status?t=${Date.now()}`);
        if (!response.ok) throw new Error("API Connection failed");
        const status = await response.json();
        updateUI(status);
    } catch (error) {
        console.error("Error fetching status:", error);
        dacSelect.style.borderColor = "var(--accent-error)";
    }
}

// Fetch all available audio interfaces and populate dropdown
async function fetchInterfaces() {
    try {
        const response = await fetch(`${API_BASE}/api/interfaces`);
        if (!response.ok) throw new Error("Failed to fetch interfaces");
        const interfaces = await response.json();
        
        // Save current selection to restore
        const currentSel = dacSelect.value || currentStatus.dac_name;
        
        // Populate dropdown
        dacSelect.innerHTML = "";
        interfaces.forEach(i => {
            const opt = document.createElement("option");
            opt.value = i.id;
            opt.textContent = i.name;
            dacSelect.appendChild(opt);
        });
        
        dacSelect.value = currentSel;
    } catch (error) {
        console.error("Error fetching interfaces:", error);
    }
}

// Send control action command
async function sendCommand(endpoint, body = {}) {
    try {
        const response = await fetch(`${API_BASE}/api/${endpoint}`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body)
        });
        if (!response.ok) throw new Error(`Command ${endpoint} failed`);
        const status = await response.json();
        updateUI(status);
    } catch (error) {
        console.error(`Error sending command ${endpoint}:`, error);
    }
}

// Update DOM elements based on state
function updateUI(status) {
    currentStatus = status;

    // 1. Sync dropdown selection
    if (dacSelect.value !== status.dac_name) {
        dacSelect.value = status.dac_name;
    }
    dacSelect.style.borderColor = "";

    // 2. Sync buffer selector (only update if not focused to avoid fighting user input)
    if (status.buffer_ms !== undefined && document.activeElement !== bufferSelect) {
        const bms = String(status.buffer_ms);
        if (bufferSelect.value !== bms) {
            // If the exact value exists as option, select it; otherwise pick closest
            const opt = [...bufferSelect.options].find(o => o.value === bms);
            if (opt) bufferSelect.value = bms;
        }
    }

    // Disable / Enable controls depending on Bluetooth, PC Audio or CD status
    if (status.bluetooth_active) {
        if (btnBluetooth) btnBluetooth.classList.add("active");
        btnPCAudio.classList.remove("active");
        btnPrev.disabled = false;
        btnPlayPause.disabled = false;
        btnStop.disabled = false;
        btnNext.disabled = false;
        btnLoad.disabled = true;
        btnEject.disabled = true;
        btnRefresh.disabled = true;
        
        cdStatusText.textContent = "BLUETOOTH LDAC";
        cdStatusText.style.color = "var(--accent-primary, #89b4fa)";
        
        if (status.bt_status === "playing") {
            cdDisc.classList.add("spinning");
            playIcon.classList.add("hidden");
            pauseIcon.classList.remove("hidden");
        } else {
            cdDisc.classList.remove("spinning");
            playIcon.classList.remove("hidden");
            pauseIcon.classList.add("hidden");
        }

        currentTrackTitle.textContent = status.bt_title || (status.bt_connected ? "Esperando audio del móvil..." : "Conecta tu móvil por Bluetooth");
        currentTrackSubtitle.textContent = status.bt_artist ? (status.bt_artist + (status.bt_album ? " — " + status.bt_album : "")) : "Audio Inalámbrico Bit-Perfect";
        
        if (status.bt_duration > 0) {
            timeElapsed.textContent = formatTime(status.bt_elapsed);
            trackTotalTime.textContent = formatTime(status.bt_duration);
            progressBarFill.style.width = `${Math.min(100, (status.bt_elapsed / status.bt_duration) * 100)}%`;
        } else {
            timeElapsed.textContent = status.bt_connected ? "CONECTADO" : "BLUETOOTH";
            trackTotalTime.textContent = "--:--";
            progressBarFill.style.width = "0%";
        }
    } else if (status.pc_audio_active) {
        if (btnBluetooth) btnBluetooth.classList.remove("active");
        btnPCAudio.classList.add("active");
        btnPrev.disabled = true;
        btnPlayPause.disabled = true;
        btnStop.disabled = true;
        btnNext.disabled = true;
        btnLoad.disabled = true;
        btnEject.disabled = true;
        btnRefresh.disabled = true;
        
        cdStatusText.textContent = "PC AUDIO";
        cdStatusText.style.color = "var(--accent-success)";
        cdDisc.classList.remove("spinning");
        playIcon.classList.remove("hidden");
        pauseIcon.classList.add("hidden");

        currentTrackTitle.textContent = "Compartir Audio PC";
        currentTrackSubtitle.textContent = "Escuchando sonido del ordenador en streaming";
        timeElapsed.textContent = "--:--";
        trackTotalTime.textContent = "--:--";
        progressBarFill.style.width = "0%";
    } else {
        if (btnBluetooth) btnBluetooth.classList.remove("active");
        btnPCAudio.classList.remove("active");
        btnPrev.disabled = false;
        btnPlayPause.disabled = false;
        btnStop.disabled = false;
        btnNext.disabled = false;
        btnLoad.disabled = false;
        btnEject.disabled = false;
        btnRefresh.disabled = false;

        // Remote Status Indicator is moved outside the else block

        // Update status text and CD spinning animation
        if (status.state === "playing") {
            cdStatusText.textContent = "REPRODUCIENDO";
            cdStatusText.style.color = "#ffffff";
            cdDisc.classList.add("spinning");
            playIcon.classList.add("hidden");
            pauseIcon.classList.remove("hidden");
        } else if (status.state === "paused") {
            cdStatusText.textContent = "PAUSADO";
            cdStatusText.style.color = "var(--text-secondary)";
            cdDisc.classList.remove("spinning");
            playIcon.classList.remove("hidden");
            pauseIcon.classList.add("hidden");
        } else if (status.state === "no_disc") {
            cdStatusText.textContent = "SIN DISCO";
            cdStatusText.style.color = "var(--accent-error)";
            cdDisc.classList.remove("spinning");
            playIcon.classList.remove("hidden");
            pauseIcon.classList.add("hidden");
        } else {
            cdStatusText.textContent = "DETENIDO";
            cdStatusText.style.color = "var(--text-secondary)";
            cdDisc.classList.remove("spinning");
            playIcon.classList.remove("hidden");
            pauseIcon.classList.add("hidden");
        }

        // Track details and metadata
        const activeTrack = status.tracks.find(t => t.track === status.current_track);
        if (status.state === "no_disc") {
            currentTrackTitle.textContent = "Sin Disco";
            currentTrackSubtitle.textContent = status.error_message || "Inserta un CD para comenzar";
            timeElapsed.textContent = "00:00";
            trackTotalTime.textContent = "00:00";
            progressBarFill.style.width = "0%";
            if (cdDisc) cdDisc.style.backgroundImage = "";
        } else if (activeTrack) {
            const trackName = activeTrack.title || `Pista ${activeTrack.track}`;
            const artistName = activeTrack.artist || status.album_artist || "";
            currentTrackTitle.textContent = trackName;
            
            let metaSub = "";
            if (artistName && status.album_title) {
                metaSub = `${artistName} — ${status.album_title}`;
            } else if (artistName) {
                metaSub = artistName;
            } else if (status.state === "playing") {
                metaSub = `Reproduciendo pista ${activeTrack.track} de ${status.tracks.length}`;
            } else if (status.state === "paused") {
                metaSub = `Pista ${activeTrack.track} en pausa`;
            } else {
                metaSub = `Pista ${activeTrack.track} seleccionada`;
            }
            currentTrackSubtitle.textContent = metaSub;
            
            if (status.cover_url && cdDisc) {
                cdDisc.style.backgroundImage = `url("${status.cover_url}")`;
                cdDisc.style.backgroundSize = "cover";
                cdDisc.style.backgroundPosition = "center";
            } else if (cdDisc) {
                cdDisc.style.backgroundImage = "";
            }
            
            // Timer and progress bar
            const elapsed = status.elapsed_time || 0;
            const total = activeTrack.duration || 1;
            timeElapsed.textContent = formatTime(elapsed);
            trackTotalTime.textContent = activeTrack.formatted_duration;
            
            const percentage = Math.min(100, (elapsed / total) * 100);
            progressBarFill.style.width = `${percentage}%`;
        } else if (status.tracks.length > 0) {
            currentTrackTitle.textContent = "CD Cargado";
            currentTrackSubtitle.textContent = `${status.tracks.length} pistas pistas listas para reproducir`;
            timeElapsed.textContent = "00:00";
            trackTotalTime.textContent = "00:00";
            progressBarFill.style.width = "0%";
        } else {
            currentTrackTitle.textContent = "Detenido";
            currentTrackSubtitle.textContent = "Presiona reproducir o selecciona una pista";
            timeElapsed.textContent = "00:00";
            trackTotalTime.textContent = "00:00";
            progressBarFill.style.width = "0%";
        }
    }

    // Dispatch global event for injected modules
    window.currentStatus = status;
    window.dispatchEvent(new CustomEvent("cdplayer:status", { detail: status }));

    // Sync Dynamic Module Injected UIs
    if (status.modules) {
        syncModuleUIs(status.modules);
    }

    // Sync Dynamic Modules UI
    if (modulesListEl && status.modules) {
        renderModules(status.modules);
    }

    // Render Tracklist
    renderTracklist(status.tracks, status.current_track, status.state);
}

// Render the list of CD tracks
function renderTracklist(tracks, currentTrack, state) {
    if (!tracks || tracks.length === 0) {
        tracklistContainer.innerHTML = `
            <div class="no-tracks-message">
                No se han detectado pistas. Inserta un CD de música y presiona "Escanear CD".
            </div>`;
        return;
    }

    // Always rebuild or update track items to reflect titles
    tracklistContainer.innerHTML = "";
    tracks.forEach(t => {
        const trackItem = document.createElement("div");
        trackItem.className = "track-item";
        trackItem.dataset.track = t.track;
        const displayTitle = t.title || `Pista ${t.track}`;
        trackItem.innerHTML = `
            <span class="track-number">${t.track.toString().padStart(2, '0')}</span>
            <span class="track-title">${displayTitle}</span>
            <span class="track-duration">${t.formatted_duration}</span>
        `;
        
        trackItem.addEventListener("click", () => {
            if (currentStatus.pc_audio_active) return;
            sendCommand("play", { track: t.track });
        });
        
        tracklistContainer.appendChild(trackItem);
    });

    // Update active highlights
    const items = tracklistContainer.querySelectorAll('.track-item');
    items.forEach(item => {
        const itemTrackNum = parseInt(item.dataset.track);
        if (itemTrackNum === currentTrack && state !== "no_disc" && state !== "idle" && !currentStatus.pc_audio_active) {
            item.classList.add("active");
        } else {
            item.classList.remove("active");
        }
    });
}

// Set up UI click event handlers
function setupEvents() {
    btnPlayPause.addEventListener("click", () => {
        if (currentStatus.state === "playing") {
            sendCommand("pause");
        } else if (currentStatus.state === "paused") {
            sendCommand("resume");
        } else {
            sendCommand("play", { track: currentStatus.current_track });
        }
    });

    btnStop.addEventListener("click", () => sendCommand("stop"));
    btnPrev.addEventListener("click", () => sendCommand("prev"));
    btnNext.addEventListener("click", () => sendCommand("next"));
    btnEject.addEventListener("click", async () => {
        btnEject.disabled = true;
        btnEject.textContent = "Expulsando...";
        await sendCommand("eject");
        btnEject.disabled = false;
        btnEject.textContent = "Expulsar";
    });

    btnLoad.addEventListener("click", async () => {
        btnLoad.disabled = true;
        btnLoad.textContent = "Cargando...";
        tracklistContainer.innerHTML = `
            <div class="no-tracks-message">
                <span class="spinner-inline"></span> Cerrando bandeja y leyendo CD... Por favor, espera.
            </div>`;
        await sendCommand("load");
        btnLoad.disabled = false;
        btnLoad.textContent = "Cargar CD";
    });

    btnRefresh.addEventListener("click", async () => {
        btnRefresh.disabled = true;
        btnRefresh.textContent = "Escaneando...";
        tracklistContainer.innerHTML = `
            <div class="no-tracks-message">
                <span class="spinner-inline"></span> Escaneando pistas del CD... Por favor, espera.
            </div>`;
        await sendCommand("refresh");
        btnRefresh.disabled = false;
        btnRefresh.textContent = "Escanear CD";
    });

    btnPCAudio.addEventListener("click", () => {
        if (currentStatus.pc_audio_active) {
            sendCommand("pc_audio/disable");
        } else {
            sendCommand("pc_audio/enable");
        }
    });

    if (btnBluetooth) {
        btnBluetooth.addEventListener("click", () => {
            if (currentStatus.bluetooth_active) {
                sendCommand("bluetooth/disable");
            } else {
                sendCommand("bluetooth/enable");
            }
        });
    }

    dacSelect.addEventListener("change", () => {
        sendCommand("select_interface", { dac_id: dacSelect.value });
    });


    bufferSelect.addEventListener("change", () => {
        const ms = parseInt(bufferSelect.value, 10);
        sendCommand("set_buffer", { buffer_ms: ms });
    });

    progressBarBg.addEventListener("click", (e) => {
        // Only allow seeking if we have tracks and player is not idle or in error/no-disc state
        if (currentStatus.pc_audio_active || !currentStatus.tracks || currentStatus.tracks.length === 0 || 
            currentStatus.state === "no_disc" || currentStatus.state === "idle" || 
            currentStatus.state === "error") {
            return;
        }

        const activeTrack = currentStatus.tracks.find(t => t.track === currentStatus.current_track);
        if (!activeTrack) return;

        // Calculate progress percentage based on click location
        const rect = progressBarBg.getBoundingClientRect();
        const clickX = e.clientX - rect.left;
        const width = rect.width;
        const percentage = Math.max(0, Math.min(1, clickX / width));

        // Send target time position in seconds
        const targetTime = percentage * activeTrack.duration;
        sendCommand("seek", { position: targetTime });
    });
}

let cachedModulesHash = "";

function renderModules(modules) {
    if (!modulesListEl) return;
    const hash = JSON.stringify(modules);
    if (hash === cachedModulesHash) return;
    cachedModulesHash = hash;

    if (!modules || modules.length === 0) {
        modulesListEl.innerHTML = '<div style="color: #6c7086; font-size: 0.9rem; padding: 0.5rem 0;">No se han detectado módulos en <code>modules/</code>.</div>';
        return;
    }

    modulesListEl.innerHTML = "";
    modules.forEach(mod => {
        const item = document.createElement("div");
        item.className = "module-item-card";
        item.style.cssText = "background: rgba(30, 30, 46, 0.4); border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 12px; padding: 0.85rem 1rem; display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 1rem;";

        const webBtn = (mod.enabled && mod.web && mod.web.enabled) ? `
            <a href="http://${window.location.hostname}:${mod.web.port || 8088}/" target="_blank" class="action-btn secondary" style="font-size: 0.8rem; padding: 0.35rem 0.75rem; text-decoration: none; border-color: #89b4fa; color: #89b4fa;">
                🌐 ${escapeHtml(mod.web.label || 'Web UI')} (:${mod.web.port || 8088})
            </a>
        ` : '';

        const capsBadges = (mod.capabilities && mod.capabilities.length > 0) ? `
            <div style="margin-top: 0.3rem; display: flex; gap: 0.35rem; flex-wrap: wrap;">
                ${mod.capabilities.map(c => `<span style="font-size: 0.65rem; background: rgba(137, 180, 250, 0.15); color: #89b4fa; border: 1px solid rgba(137, 180, 250, 0.25); padding: 0.1rem 0.4rem; border-radius: 4px; font-weight: 600;">+${escapeHtml(c)}</span>`).join('')}
            </div>
        ` : '';

        item.innerHTML = `
            <div style="display: flex; align-items: center; gap: 0.85rem; flex: 1; min-width: 260px;">
                <div style="font-size: 1.8rem; background: rgba(255, 255, 255, 0.05); width: 44px; height: 44px; display: flex; align-items: center; justify-content: center; border-radius: 10px; flex-shrink: 0;">
                    ${mod.icon || '🧩'}
                </div>
                <div>
                    <div style="display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap;">
                        <span style="font-weight: 700; color: #cdd6f4; font-size: 0.95rem;">${escapeHtml(mod.name || mod.id)}</span>
                        <span style="font-size: 0.7rem; background: rgba(203, 166, 247, 0.15); color: #cba6f7; padding: 0.15rem 0.4rem; border-radius: 4px; font-weight: 600;">v${escapeHtml(mod.version || '1.0')}</span>
                    </div>
                    <p style="margin: 0.2rem 0 0 0; font-size: 0.82rem; color: #a6adc8; line-height: 1.3;">
                        ${escapeHtml(mod.description || '')}
                    </p>
                    ${capsBadges}
                </div>
            </div>
            <div style="display: flex; align-items: center; gap: 0.6rem; flex-wrap: wrap;">
                <span style="font-size: 0.75rem; font-weight: 700; padding: 0.3rem 0.65rem; border-radius: 9999px; ${mod.enabled ? 'background: rgba(166, 227, 161, 0.18); color: #a6e3a1; border: 1px solid #a6e3a1;' : 'background: rgba(166, 173, 200, 0.12); color: #6c7086; border: 1px solid rgba(166, 173, 200, 0.2);'}">
                    ${mod.enabled ? '🟢 Activo' : '⚪ Inactivo'}
                </span>
                ${webBtn}
                <button class="action-btn ${mod.enabled ? 'danger' : 'secondary'} btn-mod-toggle" data-id="${mod.id}" style="font-size: 0.8rem; padding: 0.35rem 0.85rem; min-width: 105px;">
                    ${mod.enabled ? 'Desactivar' : 'Activar Mod'}
                </button>
            </div>
        `;

        const btn = item.querySelector(".btn-mod-toggle");
        if (btn) {
            btn.addEventListener("click", async () => {
                btn.disabled = true;
                btn.textContent = "Procesando...";
                try {
                    await sendCommand("modules/toggle", { id: mod.id, enabled: !mod.enabled });
                    cachedModulesHash = "";
                    await fetchStatus();
                } catch(e) {
                    console.error("Error toggling module:", e);
                } finally {
                    btn.disabled = false;
                }
            });
        }

        modulesListEl.appendChild(item);
    });
}

const loadedModUIs = new Set();

async function syncModuleUIs(modules) {
    if (!modules) return;
    const dashboardSlot = document.getElementById("module-slot-dashboard");
    if (!dashboardSlot) return;

    for (const mod of modules) {
        const containerId = `mod-ui-${mod.id}`;
        let container = document.getElementById(containerId);

        if (mod.enabled && mod.ui) {
            if (!container && !loadedModUIs.has(mod.id)) {
                loadedModUIs.add(mod.id);
                try {
                    const resp = await fetch(`${API_BASE}/api/modules/${mod.id}/ui`);
                    if (resp.ok) {
                        const data = await resp.json();
                        container = document.createElement("div");
                        container.id = containerId;
                        container.className = `mod-injected-ui mod-injected-${mod.id}`;
                        if (data.css) {
                            const styleEl = document.createElement("style");
                            styleEl.textContent = data.css;
                            container.appendChild(styleEl);
                        }
                        const htmlWrapper = document.createElement("div");
                        htmlWrapper.innerHTML = data.html || "";
                        container.appendChild(htmlWrapper);

                        dashboardSlot.appendChild(container);

                        if (data.js) {
                            try {
                                const fn = new Function(data.js);
                                fn();
                            } catch(err) {
                                console.error(`Error running script for module ${mod.id}:`, err);
                            }
                        }
                    }
                } catch(e) {
                    console.error(`Error loading UI for module ${mod.id}:`, e);
                    loadedModUIs.delete(mod.id);
                }
            }
        } else {
            if (container) {
                container.remove();
                loadedModUIs.delete(mod.id);
            }
        }
    }
}

// Initial initialization
async function init() {
    setupEvents();
    setupLookupModal();
    await fetchStatus();
    await fetchInterfaces();
    // Poll for status updates
    pollInterval = setInterval(fetchStatus, 1000);
}

document.addEventListener("DOMContentLoaded", init);

// ===== MusicBrainz Lookup Modal =====
const lookupModal    = document.getElementById("lookup-modal");
const modalLoading   = document.getElementById("modal-loading");
const modalError     = document.getElementById("modal-error");
const modalErrorMsg  = document.getElementById("modal-error-msg");
const modalResults   = document.getElementById("modal-results");
const modalReleases  = document.getElementById("modal-releases-list");
const btnLookup      = document.getElementById("btn-lookup");
const btnModalClose  = document.getElementById("modal-close");
const manualAlbum    = document.getElementById("manual-album");
const manualArtist   = document.getElementById("manual-artist");
const manualCover    = document.getElementById("manual-cover");
const btnApplyManual = document.getElementById("btn-apply-manual");

function openModal() {
    lookupModal.classList.remove("hidden");
    document.body.style.overflow = "hidden";
}

function closeModal() {
    lookupModal.classList.add("hidden");
    document.body.style.overflow = "";
}

function showModalState(state) {
    // state: 'loading' | 'error' | 'results'
    modalLoading.classList.toggle("hidden", state !== "loading");
    modalError.classList.toggle("hidden",   state !== "error");
    modalResults.classList.toggle("hidden", state !== "results");
}

function buildReleaseCard(release) {
    const card = document.createElement("div");
    card.className = "release-card";

    // Cover art (image with fallback to placeholder)
    let coverEl;
    if (release.cover_url) {
        coverEl = document.createElement("img");
        coverEl.className = "release-cover";
        coverEl.src = release.cover_url;
        coverEl.alt = "Portada";
        coverEl.onerror = () => {
            const ph = document.createElement("div");
            ph.className = "release-cover-placeholder";
            ph.textContent = "💿";
            coverEl.replaceWith(ph);
        };
    } else {
        coverEl = document.createElement("div");
        coverEl.className = "release-cover-placeholder";
        coverEl.textContent = "💿";
    }

    // Info block
    const info = document.createElement("div");
    info.className = "release-info";

    const trackPreview = (release.tracks || []).slice(0, 4)
        .map(t => `${t.track}. ${t.title}`).join(" · ");

    // Disc badge: only show when it's a multi-disc release
    const discBadge = (release.disc_total > 1)
        ? `<span class="disc-badge">💿 CD ${release.disc_number} de ${release.disc_total}</span>`
        : "";

    info.innerHTML = `
        <div class="release-title">${escapeHtml(release.title)} ${discBadge}</div>
        <div class="release-artist">${escapeHtml(release.artist || "Artista desconocido")}</div>
        <div class="release-year">${release.year || ""}</div>
        ${trackPreview ? `<div class="release-tracks-preview">${escapeHtml(trackPreview)}${release.tracks.length > 4 ? " ···" : ""}</div>` : ""}
        <button class="release-apply-btn" data-idx="">✓ Aplicar esta edición</button>
    `;

    card.appendChild(coverEl);
    card.appendChild(info);

    // Click on card or button applies the release
    const applyBtn = info.querySelector(".release-apply-btn");
    const doApply = () => applyRelease(release);
    applyBtn.addEventListener("click", (e) => { e.stopPropagation(); doApply(); });
    card.addEventListener("click", doApply);

    return card;
}

function escapeHtml(str) {
    return String(str)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;");
}

async function applyRelease(release) {
    try {
        const body = {
            title:     release.title,
            artist:    release.artist,
            cover_url: release.cover_url,
            tracks:    release.tracks,
        };
        const resp = await fetch(`${API_BASE}/api/set_release`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body),
        });
        if (!resp.ok) throw new Error("set_release failed");
        const status = await resp.json();
        updateUI(status);
        closeModal();
    } catch(e) {
        console.error("Error applying release:", e);
    }
}

async function runLookup() {
    openModal();
    showModalState("loading");
    // Pre-fill manual fields with current metadata
    manualAlbum.value  = currentStatus.album_title  || "";
    manualArtist.value = currentStatus.album_artist || "";
    manualCover.value  = currentStatus.cover_url    || "";

    try {
        const resp = await fetch(`${API_BASE}/api/lookup`);
        if (!resp.ok) throw new Error("lookup HTTP error");
        const data = await resp.json();

        if (data.error && (!data.releases || data.releases.length === 0)) {
            modalErrorMsg.textContent = data.error;
            showModalState("error");
            return;
        }

        modalReleases.innerHTML = "";
        (data.releases || []).forEach(rel => {
            modalReleases.appendChild(buildReleaseCard(rel));
        });
        showModalState("results");

    } catch(e) {
        modalErrorMsg.textContent = `Error de red: ${e.message}`;
        showModalState("error");
    }
}

function setupLookupModal() {
    btnLookup.addEventListener("click", runLookup);

    btnModalClose.addEventListener("click", closeModal);

    // Close on overlay click
    lookupModal.addEventListener("click", (e) => {
        if (e.target === lookupModal) closeModal();
    });

    // Keyboard: Esc closes
    document.addEventListener("keydown", (e) => {
        if (e.key === "Escape" && !lookupModal.classList.contains("hidden")) {
            closeModal();
        }
    });

    // Manual apply
    btnApplyManual.addEventListener("click", async () => {
        const release = {
            title:     manualAlbum.value.trim(),
            artist:    manualArtist.value.trim(),
            cover_url: manualCover.value.trim(),
            tracks:    [],
        };
        await applyRelease(release);
    });
}
