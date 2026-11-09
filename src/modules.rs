use crate::playback::stop_playback_unsafe;
use crate::state::{ModuleInfo, ModuleUiConfig, ModuleUiData, ModuleWebConfig, PlayerState};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Arc;
use tokio::sync::RwLock;

pub const MODULES_CONFIG_FILE: &str = "/etc/cdplayer_modules.json";
pub const LOCAL_MODULES_CONFIG_FILE: &str = "modules_config.json";

pub const LOFI_YOUTUBE_URL: &str = "https://youtu.be/rFZHOHl-L8A";
pub const WHITENOISE_YOUTUBE_URL: &str = "https://youtu.be/nMfPqeZjc2c";

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ModulesConfigFile {
    pub enabled: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct RawModuleManifest {
    pub id: Option<String>,
    pub name: Option<String>,
    pub version: Option<String>,
    pub description: Option<String>,
    pub author: Option<String>,
    pub icon: Option<String>,
    pub capabilities: Option<Vec<String>>,
    pub service: Option<String>,
    pub web: Option<ModuleWebConfig>,
    pub ui: Option<ModuleUiConfig>,
}

pub struct ModuleManager {
    modules_dir: PathBuf,
    config_file: PathBuf,
    manifests: Arc<RwLock<HashMap<String, ModuleInfo>>>,
    enabled_modules: Arc<RwLock<HashSet<String>>>,
    study_stream_child: Arc<std::sync::Mutex<Option<Child>>>,
}

impl ModuleManager {
    pub fn new(modules_dir: PathBuf) -> Self {
        let config_file = if Path::new(MODULES_CONFIG_FILE).exists()
            || Path::new("/etc").metadata().map(|m| !m.permissions().readonly()).unwrap_or(false)
        {
            PathBuf::from(MODULES_CONFIG_FILE)
        } else {
            PathBuf::from(LOCAL_MODULES_CONFIG_FILE)
        };

        let mut enabled = HashSet::new();
        for p in [&config_file, &PathBuf::from(LOCAL_MODULES_CONFIG_FILE)] {
            if let Ok(content) = fs::read_to_string(p) {
                if let Ok(data) = serde_json::from_str::<ModulesConfigFile>(&content) {
                    enabled = data.enabled.into_iter().collect();
                    break;
                }
            }
        }

        let mgr = Self {
            modules_dir,
            config_file,
            manifests: Arc::new(RwLock::new(HashMap::new())),
            enabled_modules: Arc::new(RwLock::new(enabled)),
            study_stream_child: Arc::new(std::sync::Mutex::new(None)),
        };

        mgr.discover_modules_sync();
        mgr.sync_active_services_sync();
        mgr
    }

    pub fn discover_modules_sync(&self) {
        let mut map = HashMap::new();
        let enabled = self.enabled_modules.blocking_read().clone();

        if let Ok(entries) = fs::read_dir(&self.modules_dir) {
            for entry in entries.flatten() {
                let mod_path = entry.path();
                let manifest_path = mod_path.join("module.json");
                if mod_path.is_dir() && manifest_path.is_file() {
                    if let Ok(content) = fs::read_to_string(&manifest_path) {
                        if let Ok(raw) = serde_json::from_str::<RawModuleManifest>(&content) {
                            let dir_name = mod_path
                                .file_name()
                                .and_then(|n| n.to_str())
                                .unwrap_or_default()
                                .to_string();
                            let mod_id = raw.id.unwrap_or(dir_name);
                            let is_enabled = enabled.contains(&mod_id);

                            map.insert(
                                mod_id.clone(),
                                ModuleInfo {
                                    id: mod_id,
                                    name: raw.name.unwrap_or_default(),
                                    version: raw.version.unwrap_or_else(|| "1.0.0".to_string()),
                                    description: raw.description.unwrap_or_default(),
                                    author: raw.author.unwrap_or_default(),
                                    icon: raw.icon.unwrap_or_else(|| "🧩".to_string()),
                                    capabilities: raw.capabilities.unwrap_or_default(),
                                    enabled: is_enabled,
                                    service: raw.service,
                                    web: raw.web,
                                    ui: raw.ui,
                                },
                            );
                        }
                    }
                }
            }
        }

        *self.manifests.blocking_write() = map;
    }

    pub async fn discover_modules(&self) {
        let mut map = HashMap::new();
        let enabled = self.enabled_modules.read().await.clone();

        if let Ok(entries) = fs::read_dir(&self.modules_dir) {
            for entry in entries.flatten() {
                let mod_path = entry.path();
                let manifest_path = mod_path.join("module.json");
                if mod_path.is_dir() && manifest_path.is_file() {
                    if let Ok(content) = fs::read_to_string(&manifest_path) {
                        if let Ok(raw) = serde_json::from_str::<RawModuleManifest>(&content) {
                            let dir_name = mod_path
                                .file_name()
                                .and_then(|n| n.to_str())
                                .unwrap_or_default()
                                .to_string();
                            let mod_id = raw.id.unwrap_or(dir_name);
                            let is_enabled = enabled.contains(&mod_id);

                            map.insert(
                                mod_id.clone(),
                                ModuleInfo {
                                    id: mod_id,
                                    name: raw.name.unwrap_or_default(),
                                    version: raw.version.unwrap_or_else(|| "1.0.0".to_string()),
                                    description: raw.description.unwrap_or_default(),
                                    author: raw.author.unwrap_or_default(),
                                    icon: raw.icon.unwrap_or_else(|| "🧩".to_string()),
                                    capabilities: raw.capabilities.unwrap_or_default(),
                                    enabled: is_enabled,
                                    service: raw.service,
                                    web: raw.web,
                                    ui: raw.ui,
                                },
                            );
                        }
                    }
                }
            }
        }

        *self.manifests.write().await = map;
    }

    pub async fn list_modules(&self, refresh: bool) -> Vec<ModuleInfo> {
        if refresh {
            self.discover_modules().await;
        }
        let map = self.manifests.read().await;
        let mut list: Vec<ModuleInfo> = map.values().cloned().collect();
        list.sort_by(|a, b| a.id.cmp(&b.id));
        list
    }

    pub async fn has_capability(&self, cap: &str) -> bool {
        let enabled = self.enabled_modules.read().await;
        let manifests = self.manifests.read().await;
        for mod_id in enabled.iter() {
            if let Some(m) = manifests.get(mod_id) {
                if m.capabilities.iter().any(|c| c == cap) {
                    return true;
                }
            }
        }
        false
    }

    pub async fn get_active_capabilities(&self) -> Vec<String> {
        let enabled = self.enabled_modules.read().await;
        let manifests = self.manifests.read().await;
        let mut caps = HashSet::new();
        for mod_id in enabled.iter() {
            if let Some(m) = manifests.get(mod_id) {
                for c in &m.capabilities {
                    caps.insert(c.clone());
                }
            }
        }
        let mut list: Vec<String> = caps.into_iter().collect();
        list.sort();
        list
    }

    pub async fn get_module_ui(&self, mod_id: &str) -> Option<ModuleUiData> {
        let manifests = self.manifests.read().await;
        let m = manifests.get(mod_id)?;
        let ui_cfg = m.ui.as_ref()?;

        let mod_path = self.modules_dir.join(mod_id);
        let html_file = mod_path.join(ui_cfg.html.as_deref().unwrap_or("ui.html"));
        let js_file = mod_path.join(ui_cfg.js.as_deref().unwrap_or("ui.js"));
        let css_file = mod_path.join(ui_cfg.css.as_deref().unwrap_or("ui.css"));

        let html = fs::read_to_string(html_file).unwrap_or_default();
        let js = fs::read_to_string(js_file).unwrap_or_default();
        let css = fs::read_to_string(css_file).unwrap_or_default();

        Some(ModuleUiData {
            id: mod_id.to_string(),
            slot: ui_cfg.slot.clone().unwrap_or_else(|| "dashboard".to_string()),
            html,
            js,
            css,
        })
    }

    pub async fn toggle_module(&self, mod_id: &str, enable: Option<bool>) -> (bool, bool) {
        self.discover_modules().await;
        let mut manifests = self.manifests.write().await;
        if !manifests.contains_key(mod_id) {
            return (false, false);
        }

        let mut enabled = self.enabled_modules.write().await;
        let cur = enabled.contains(mod_id);
        let target = enable.unwrap_or(!cur);

        if target {
            enabled.insert(mod_id.to_string());
        } else {
            enabled.remove(mod_id);
        }

        if let Some(m) = manifests.get_mut(mod_id) {
            m.enabled = target;
            if let Some(ref service) = m.service {
                let act = if target { "start" } else { "stop" };
                let _ = Command::new("systemctl").args([act, service]).spawn();
            }

            let hook_script = self.modules_dir.join(mod_id).join("hooks.py");
            if hook_script.is_file() {
                let act = if target { "enable" } else { "disable" };
                let _ = Command::new("python3")
                    .args([hook_script.to_str().unwrap_or("hooks.py"), act])
                    .current_dir(self.modules_dir.join(mod_id))
                    .spawn();
            }
        }

        self.save_config(&enabled);
        (true, target)
    }

    fn save_config(&self, enabled: &HashSet<String>) {
        let mut sorted: Vec<String> = enabled.iter().cloned().collect();
        sorted.sort();
        let cfg = ModulesConfigFile { enabled: sorted };
        if let Ok(json) = serde_json::to_string_pretty(&cfg) {
            for p in [&self.config_file, &PathBuf::from(LOCAL_MODULES_CONFIG_FILE)] {
                if fs::write(p, &json).is_ok() {
                    break;
                }
            }
        }
    }

    pub fn sync_active_services_sync(&self) {
        let enabled = self.enabled_modules.blocking_read();
        let manifests = self.manifests.blocking_read();
        for (mod_id, m) in manifests.iter() {
            if let Some(ref service) = m.service {
                let act = if enabled.contains(mod_id) {
                    "start"
                } else {
                    "stop"
                };
                let _ = Command::new("systemctl").args([act, service]).spawn();
            }
        }
    }

    // --- Study Mode Native Backend ---
    pub fn stop_study_stream(&self, state: Arc<RwLock<PlayerState>>) {
        let mut lock = self.study_stream_child.lock().unwrap();
        if let Some(mut child) = lock.take() {
            let _ = child.kill();
            let _ = child.wait();
        }

        let _ = Command::new("pkill")
            .args(["-f", "ffmpeg.*rFZHOHl-L8A"])
            .output();
        let _ = Command::new("pkill")
            .args(["-f", "ffmpeg.*nMfPqeZjc2c"])
            .output();
        let _ = Command::new("pkill")
            .args(["-f", "ffmpeg.*googlevideo"])
            .output();

        let s_clone = state.clone();
        tokio::spawn(async move {
            let mut s = s_clone.write().await;
            s.study_lofi_active = false;
            s.study_whitenoise_active = false;
        });
    }

    pub async fn start_study_stream(
        &self,
        audio_type: &str,
        state: Arc<RwLock<PlayerState>>,
    ) -> bool {
        self.stop_study_stream(state.clone());
        stop_playback_unsafe(state.clone()).await;

        let yt_url = if audio_type == "whitenoise" {
            WHITENOISE_YOUTUBE_URL
        } else {
            LOFI_YOUTUBE_URL
        };

        let tag = if audio_type == "whitenoise" {
            "[RUIDO-BLANCO]"
        } else {
            "[LOFI]"
        };

        let yt_cmd = Command::new("/usr/local/bin/yt-dlp")
            .args(["-g", "-f", "bestaudio", yt_url])
            .output();

        let stream_url = match yt_cmd {
            Ok(out) => {
                let stdout = String::from_utf8_lossy(&out.stdout);
                stdout
                    .lines()
                    .map(|l| l.trim())
                    .find(|l| l.starts_with("http"))
                    .unwrap_or_default()
                    .to_string()
            }
            Err(e) => {
                tracing::error!("{} yt-dlp error: {}", tag, e);
                String::new()
            }
        };

        if stream_url.is_empty() {
            let mut s = state.write().await;
            s.study_lofi_active = false;
            s.study_whitenoise_active = false;
            return false;
        }

        let has_audio = Path::new("/proc/asound/AUDIO").exists();
        let has_head = Path::new("/proc/asound/Headphones").exists();

        let mut ff_args = vec!["-re", "-loglevel", "warning", "-i", &stream_url];
        let filter_split = "[0:a]asplit=2[a1][a2]";
        let alsa_audio = "plughw:CARD=AUDIO,DEV=0";
        let alsa_head = "plughw:CARD=Headphones,DEV=0";

        if has_audio && has_head {
            ff_args.extend([
                "-filter_complex",
                filter_split,
                "-map",
                "[a1]",
                "-f",
                "alsa",
                alsa_audio,
                "-map",
                "[a2]",
                "-f",
                "alsa",
                alsa_head,
            ]);
        } else if has_head {
            ff_args.extend(["-f", "alsa", alsa_head]);
        } else if has_audio {
            ff_args.extend(["-f", "alsa", alsa_audio]);
        } else {
            ff_args.extend(["-f", "alsa", "default"]);
        }

        match Command::new("ffmpeg")
            .args(&ff_args)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            Ok(child) => {
                {
                    let mut lock = self.study_stream_child.lock().unwrap();
                    *lock = Some(child);
                }

                let mut s = state.write().await;
                if audio_type == "whitenoise" {
                    s.study_whitenoise_active = true;
                    s.study_lofi_active = false;
                } else {
                    s.study_lofi_active = true;
                    s.study_whitenoise_active = false;
                }
                true
            }
            Err(e) => {
                tracing::error!("{} Failed to start ffmpeg: {}", tag, e);
                let mut s = state.write().await;
                s.study_lofi_active = false;
                s.study_whitenoise_active = false;
                false
            }
        }
    }
}
