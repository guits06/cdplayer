use serde::{Deserialize, Serialize};
use std::time::Instant;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackInfo {
    pub track: i32,
    pub title: String,
    pub artist: String,
    pub start_lba: i32,
    pub start_sec: f64,
    pub total_sectors: i32,
    pub duration: i32,
    pub formatted_duration: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AudioInterface {
    pub id: String,
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleWebConfig {
    pub enabled: bool,
    pub port: u16,
    pub label: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleUiConfig {
    pub slot: Option<String>,
    pub html: Option<String>,
    pub js: Option<String>,
    pub css: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleInfo {
    pub id: String,
    pub name: String,
    pub version: String,
    pub description: String,
    pub author: String,
    pub icon: String,
    pub capabilities: Vec<String>,
    pub enabled: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub service: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub web: Option<ModuleWebConfig>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ui: Option<ModuleUiConfig>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleUiData {
    pub id: String,
    pub slot: String,
    pub html: String,
    pub js: String,
    pub css: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PlayerStateSnapshot {
    pub state: String,
    pub current_track: i32,
    pub tracks: Vec<TrackInfo>,
    pub dac_name: String,
    pub elapsed_time: f64,
    pub error_message: String,
    pub pc_audio_active: bool,
    pub buffer_ms: i32,
    pub buffer_fill_pct: i32,
    pub album_title: String,
    pub album_artist: String,
    pub cover_url: String,
    pub bluetooth_active: bool,
    pub bt_mode: String,
    pub bt_connected: bool,
    pub bt_device_name: String,
    pub bt_title: String,
    pub bt_artist: String,
    pub bt_album: String,
    pub bt_status: String,
    pub bt_duration: f64,
    pub bt_elapsed: f64,
    pub study_mode_active: bool,
    pub study_lofi_active: bool,
    pub study_whitenoise_active: bool,
    pub aux_volume: i32,
    pub vol_l: i32,
    pub vol_r: i32,
    pub mute_l: bool,
    pub mute_r: bool,
    pub bit_perfect: bool,
    pub voip_module_enabled: bool,
    pub study_module_enabled: bool,
    pub capabilities: Vec<String>,
    pub modules: Vec<ModuleInfo>,
}

#[allow(dead_code)]
pub struct PlayerState {
    pub state: String,
    pub current_track: i32,
    pub tracks: Vec<TrackInfo>,
    pub dac_name: String,
    pub manual_dac: bool,
    pub error_message: String,
    pub pc_audio_active: bool,
    pub buffer_ms: i32,
    pub buffer_fill_pct: i32,
    pub pause_position: f64,
    pub album_title: String,
    pub album_artist: String,
    pub cover_url: String,
    pub toc_offsets: Vec<(i32, i32)>,
    pub toc_leadout: i32,
    pub recovery_attempts: i32,

    pub bytes_played: u64,
    pub play_start_time: Option<Instant>,
    pub seek_offset_time: f64,
    pub session_id: u64,
    pub last_eject_time: Instant,

    // Bluetooth
    pub bluetooth_active: bool,
    pub bt_mode: String,
    pub bt_connected: bool,
    pub bt_device_name: String,
    pub bt_title: String,
    pub bt_artist: String,
    pub bt_album: String,
    pub bt_status: String,
    pub bt_duration: f64,
    pub bt_elapsed: f64,

    // Study mode
    pub study_mode_active: bool,
    pub study_lofi_active: bool,
    pub study_whitenoise_active: bool,
    pub aux_volume: i32,
    pub study_bg_current: String,

    // Volumes and mutes
    pub vol_l: i32,
    pub vol_r: i32,
    pub mute_l: bool,
    pub mute_r: bool,

    // Subprocess handles (if any)
    pub pc_audio_proc: Option<std::process::Child>,
    pub fallback_p_read: Option<std::process::Child>,
    pub fallback_p_play: Option<std::process::Child>,
    pub study_audio_proc: Option<std::process::Child>,
}

impl PlayerState {
    pub fn new() -> Self {
        Self {
            state: "idle".to_string(),
            current_track: 1,
            tracks: Vec::new(),
            dac_name: "default".to_string(),
            manual_dac: false,
            error_message: String::new(),
            pc_audio_active: false,
            buffer_ms: 2500,
            buffer_fill_pct: 0,
            pause_position: 0.0,
            album_title: String::new(),
            album_artist: String::new(),
            cover_url: String::new(),
            toc_offsets: Vec::new(),
            toc_leadout: 0,
            recovery_attempts: 0,

            bytes_played: 0,
            play_start_time: None,
            seek_offset_time: 0.0,
            session_id: 0,
            last_eject_time: Instant::now() - std::time::Duration::from_secs(60),

            bluetooth_active: false,
            bt_mode: "off".to_string(),
            bt_connected: false,
            bt_device_name: String::new(),
            bt_title: String::new(),
            bt_artist: String::new(),
            bt_album: String::new(),
            bt_status: "stopped".to_string(),
            bt_duration: 0.0,
            bt_elapsed: 0.0,

            study_mode_active: false,
            study_lofi_active: false,
            study_whitenoise_active: false,
            aux_volume: 80,
            study_bg_current: String::new(),

            vol_l: 100,
            vol_r: 100,
            mute_l: false,
            mute_r: false,

            pc_audio_proc: None,
            fallback_p_read: None,
            fallback_p_play: None,
            study_audio_proc: None,
        }
    }

    pub fn snapshot(
        &self,
        modules: Vec<ModuleInfo>,
        active_caps: Vec<String>,
        has_voip: bool,
        has_study: bool,
    ) -> PlayerStateSnapshot {
        let cur_dur = if self.current_track > 0 && (self.current_track as usize) <= self.tracks.len() {
            self.tracks[(self.current_track - 1) as usize].duration as f64
        } else {
            0.0
        };

        let mut elapsed = (self.bytes_played as f64) / 176400.0;
        if self.state == "playing" {
            if let Some(start) = self.play_start_time {
                elapsed = (start.elapsed().as_secs_f64() + self.seek_offset_time).max(0.0);
            }
        }
        if cur_dur > 0.0 {
            elapsed = elapsed.min(cur_dur);
        }

        let bit_perfect = self.vol_l >= 100 && self.vol_r >= 100 && !self.mute_l && !self.mute_r;

        PlayerStateSnapshot {
            state: self.state.clone(),
            current_track: self.current_track,
            tracks: self.tracks.clone(),
            dac_name: self.dac_name.clone(),
            elapsed_time: elapsed,
            error_message: self.error_message.clone(),
            pc_audio_active: self.pc_audio_active,
            buffer_ms: self.buffer_ms,
            buffer_fill_pct: self.buffer_fill_pct,
            album_title: self.album_title.clone(),
            album_artist: self.album_artist.clone(),
            cover_url: self.cover_url.clone(),
            bluetooth_active: self.bluetooth_active,
            bt_mode: self.bt_mode.clone(),
            bt_connected: self.bt_connected,
            bt_device_name: self.bt_device_name.clone(),
            bt_title: self.bt_title.clone(),
            bt_artist: self.bt_artist.clone(),
            bt_album: self.bt_album.clone(),
            bt_status: self.bt_status.clone(),
            bt_duration: self.bt_duration,
            bt_elapsed: self.bt_elapsed,
            study_mode_active: self.study_mode_active,
            study_lofi_active: self.study_lofi_active,
            study_whitenoise_active: self.study_whitenoise_active,
            aux_volume: self.aux_volume,
            vol_l: self.vol_l,
            vol_r: self.vol_r,
            mute_l: self.mute_l,
            mute_r: self.mute_r,
            bit_perfect,
            voip_module_enabled: has_voip,
            study_module_enabled: has_study,
            capabilities: active_caps,
            modules,
        }
    }
}
