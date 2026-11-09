use crate::cdda_ffi::{self, c_char_to_string, CDDiscInfoC};
use crate::hardware::{check_cd_inserted, get_cd_device, set_drive_speed};
use crate::musicbrainz::query_musicbrainz;
use crate::playback::start_playback_unsafe;
use crate::state::{PlayerState, TrackInfo};
use std::collections::HashMap;
use std::ffi::CString;
use std::path::Path;
use std::process::Command;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::RwLock;

pub async fn auto_enrich_musicbrainz(
    expected_leadout: i32,
    state_ref: Arc<RwLock<PlayerState>>,
) {
    let (offsets, actual_durations, num_tracks) = {
        let s = state_ref.read().await;
        let mut actual = HashMap::new();
        for t in &s.tracks {
            actual.insert(t.track, t.duration);
        }
        (s.toc_offsets.clone(), actual, s.tracks.len() as i32)
    };

    let res = query_musicbrainz(&offsets, expected_leadout, &actual_durations).await;
    if res.releases.is_empty() {
        return;
    }

    let best = &res.releases[0];
    let max_allowed_diff = (num_tracks * 4).max(15);
    if best.duration_diff > max_allowed_diff && res.releases.len() > 1 {
        tracing::info!(
            "[MusicBrainz] Mejor candidato '{}' supera umbral ({}s > {}s).",
            best.title,
            best.duration_diff,
            max_allowed_diff
        );
        return;
    }

    let mut s = state_ref.write().await;
    if s.toc_leadout != expected_leadout || s.tracks.is_empty() {
        return;
    }
    if !s.album_title.is_empty() {
        return;
    }

    s.album_title = if !best.title.is_empty() {
        best.title.clone()
    } else {
        s.album_title.clone()
    };
    s.album_artist = if !best.artist.is_empty() {
        best.artist.clone()
    } else {
        s.album_artist.clone()
    };
    s.cover_url = if !best.cover_url.is_empty() {
        best.cover_url.clone()
    } else {
        s.cover_url.clone()
    };

    let mb_tracks: HashMap<i32, &crate::musicbrainz::MbTrackInfo> =
        best.tracks.iter().map(|t| (t.track, t)).collect();

    for tr in &mut s.tracks {
        if let Some(mb_t) = mb_tracks.get(&tr.track) {
            if !mb_t.title.is_empty() {
                tr.title = mb_t.title.clone();
                if let Ok(c_title) = CString::new(tr.title.clone()) {
                    unsafe {
                        cdda_ffi::cdda_set_track_title(tr.track, c_title.as_ptr());
                    }
                }
            }
            if !mb_t.artist.is_empty() {
                tr.artist = mb_t.artist.clone();
            }
        }
    }

    tracing::info!(
        "[MusicBrainz] Metadatos aplicados automáticamente: {} - {} (diff={}s)",
        s.album_artist,
        s.album_title,
        best.duration_diff
    );
}

pub fn get_track_list(
    state_ref: Option<Arc<RwLock<PlayerState>>>,
) -> (Vec<TrackInfo>, String, String, i32, Vec<(i32, i32)>) {
    let dev = get_cd_device();
    if !Path::new(&dev).exists() {
        return (Vec::new(), String::new(), String::new(), 0, Vec::new());
    }

    set_drive_speed(2);

    let mut tracks = Vec::new();
    let mut raw_entries = Vec::new();
    let mut leadout_lba = 0;
    let mut album_artist = String::new();
    let mut album_title = String::new();

    // 1. Native C Engine TOC + SCSI CD-TEXT read (< 10 ms)
    if let Ok(c_dev) = CString::new(dev.clone()) {
        unsafe {
            cdda_ffi::cdda_set_device(c_dev.as_ptr());
            let mut info = CDDiscInfoC::default();
            let count = cdda_ffi::cdda_read_disc_info(&mut info);
            if count > 0 {
                leadout_lba = info.leadout_lba;
                let c_album = c_char_to_string(&info.album_title);
                let c_art = c_char_to_string(&info.album_artist);
                if !c_album.is_empty() && c_album != "Audio CD" {
                    album_title = c_album;
                }
                if !c_art.is_empty() && c_art != "Unknown Artist" {
                    album_artist = c_art;
                }

                for i in 0..(count as usize) {
                    let t = &info.tracks[i];
                    let dur = t.duration_sec;
                    let mins = dur / 60;
                    let secs = dur % 60;
                    let mut t_title = c_char_to_string(&t.title);
                    let t_artist = c_char_to_string(&t.artist);
                    if t_title.is_empty() || t_title.starts_with("Track ") {
                        t_title = format!("Pista {}", t.track_num);
                    }
                    raw_entries.push((t.track_num, t.start_lba));
                    tracks.push(TrackInfo {
                        track: t.track_num,
                        title: t_title,
                        artist: t_artist,
                        start_lba: t.start_lba,
                        start_sec: (t.start_lba as f64) / 75.0,
                        total_sectors: t.total_sectors,
                        duration: dur,
                        formatted_duration: format!("{:02}:{:02}", mins, secs),
                    });
                }
            }
        }
    }

    // 2. Fallback to cd-info if native C engine produced 0 tracks
    if tracks.is_empty() {
        if let Ok(out) = Command::new("cd-info")
            .args([
                "--no-device-info",
                "--no-disc-mode",
                "--no-analyze",
                "-T",
                "-C",
                &dev,
            ])
            .output()
        {
            let text = String::from_utf8_lossy(&out.stdout);
            let mut track_titles: HashMap<i32, String> = HashMap::new();
            let mut track_performers: HashMap<i32, String> = HashMap::new();
            let mut in_cdtext = false;
            let mut cur_tr = None;

            for line in text.lines() {
                let trimmed = line.trim();
                if trimmed.starts_with("CD-TEXT for Disc:") {
                    in_cdtext = true;
                    continue;
                }
                if let Some(pos) = trimmed.find("CD-TEXT for Track ") {
                    let rest = &trimmed[pos + 18..];
                    if let Some(col) = rest.find(':') {
                        if let Ok(tn) = rest[..col].trim().parse::<i32>() {
                            cur_tr = Some(tn);
                            continue;
                        }
                    }
                }
                if in_cdtext {
                    if let Some(title_pos) = trimmed.find("TITLE:") {
                        let val = trimmed[title_pos + 6..].trim().to_string();
                        if let Some(tn) = cur_tr {
                            track_titles.insert(tn, val);
                        } else if album_title.is_empty() {
                            album_title = val;
                        }
                    }
                    if let Some(perf_pos) = trimmed.find("PERFORMER:") {
                        let val = trimmed[perf_pos + 10..].trim().to_string();
                        if let Some(tn) = cur_tr {
                            track_performers.insert(tn, val);
                        } else if album_artist.is_empty() {
                            album_artist = val;
                        }
                    }
                }

                // Parse track lines: e.g. " 1: 00:02:00 0 audio"
                let parts: Vec<&str> = line.split_whitespace().collect();
                if parts.len() >= 4 {
                    if let Some(tr_str) = parts[0].strip_suffix(':') {
                        if let (Ok(tn), Ok(lba)) = (tr_str.parse::<i32>(), parts[2].parse::<i32>())
                        {
                            let t_type = parts[3];
                            if t_type == "audio" && tn < 100 {
                                raw_entries.push((tn, lba));
                            } else if t_type == "leadout" || tn >= 100 {
                                leadout_lba = lba;
                            }
                        }
                    }
                }
            }

            if !raw_entries.is_empty() {
                for i in 0..raw_entries.len() {
                    let (tn, start_lba) = raw_entries[i];
                    let next_lba = if i + 1 < raw_entries.len() {
                        raw_entries[i + 1].1
                    } else {
                        leadout_lba
                    };
                    let total_sectors = (next_lba - start_lba).max(75);
                    let dur = total_sectors / 75;
                    let mins = dur / 60;
                    let secs = dur % 60;

                    tracks.push(TrackInfo {
                        track: tn,
                        title: track_titles
                            .get(&tn)
                            .cloned()
                            .unwrap_or_else(|| format!("Pista {}", tn)),
                        artist: track_performers.get(&tn).cloned().unwrap_or_default(),
                        start_lba,
                        start_sec: (start_lba as f64) / 75.0,
                        total_sectors,
                        duration: dur,
                        formatted_duration: format!("{:02}:{:02}", mins, secs),
                    });
                }
            }
        }
    }

    if let Some(sr) = state_ref {
        let sr_clone = sr.clone();
        let leadout_capture = leadout_lba;
        let has_tracks = !tracks.is_empty();
        let no_title = album_title.is_empty();

        let raw_entries_clone = raw_entries.clone();
        let album_artist_clone = album_artist.clone();
        let album_title_clone = album_title.clone();

        tokio::spawn(async move {
            {
                let mut s = sr_clone.write().await;
                s.toc_offsets = raw_entries_clone;
                s.toc_leadout = leadout_capture;
                s.album_artist = album_artist_clone;
                s.album_title = album_title_clone;
                s.cover_url.clear();
            }

            if has_tracks && no_title && leadout_capture > 0 {
                auto_enrich_musicbrainz(leadout_capture, sr_clone).await;
            }
        });
    }

    (tracks, album_title, album_artist, leadout_lba, raw_entries)
}

pub fn spawn_cd_insertion_monitor(state_ref: Arc<RwLock<PlayerState>>) {
    tokio::spawn(async move {
        let mut last_disc_present = false;
        let mut toc_failed_since: Option<Instant> = None;

        loop {
            tokio::time::sleep(Duration::from_secs(3)).await;

            let should_skip = {
                let s = state_ref.read().await;
                if s.last_eject_time.elapsed() < Duration::from_secs(6) {
                    last_disc_present = false;
                    toc_failed_since = None;
                    true
                } else if s.state == "playing" || s.state == "starting" || s.state == "paused" {
                    last_disc_present = true;
                    toc_failed_since = None;
                    true
                } else {
                    false
                }
            };

            if should_skip {
                continue;
            }

            let disc_present = check_cd_inserted(last_disc_present);

            if !disc_present {
                toc_failed_since = None;
                let mut s = state_ref.write().await;
                if s.state != "pc_audio" && s.state != "bluetooth" && (last_disc_present || s.state != "no_disc") {
                    s.tracks.clear();
                    s.album_title.clear();
                    s.album_artist.clear();
                    s.cover_url.clear();
                    s.state = "no_disc".to_string();
                }
            } else if disc_present && !last_disc_present {
                set_drive_speed(2);
                tracing::info!("Disco detectado en el lector. Interrumpiendo otros modos y cargando CD...");

                let mut s = state_ref.write().await;
                s.pc_audio_active = false;
                if let Some(mut child) = s.pc_audio_proc.take() {
                    let _ = child.kill();
                }
                s.state = "idle".to_string();
                toc_failed_since = Some(Instant::now());
            }

            if disc_present && (!last_disc_present || toc_failed_since.is_some()) {
                let (tracks, title, artist, leadout, raw_offsets) = get_track_list(None);
                if !tracks.is_empty() {
                    let leadout_copy = leadout;
                    let no_title = title.is_empty();

                    {
                        let mut s = state_ref.write().await;
                        s.tracks = tracks;
                        s.current_track = 1;
                        s.error_message.clear();
                        s.state = "starting".to_string();
                        s.album_title = title;
                        s.album_artist = artist;
                        s.cover_url.clear();
                        s.toc_offsets = raw_offsets;
                        s.toc_leadout = leadout;
                    }

                    tracing::info!("Disco cargado correctamente. Iniciando reproducción automática (Pista 1)...");
                    toc_failed_since = None;

                    start_playback_unsafe(state_ref.clone(), 1, 0.0).await;

                    if no_title && leadout_copy > 0 {
                        let sr = state_ref.clone();
                        tokio::spawn(async move {
                            auto_enrich_musicbrainz(leadout_copy, sr).await;
                        });
                    }
                } else {
                    let elapsed = toc_failed_since
                        .map(|t| t.elapsed().as_secs())
                        .unwrap_or(0);
                    if elapsed > 30 {
                        tracing::warn!("No se pudo leer TOC tras 30s. Disco incompatible o sin datos.");
                        let mut s = state_ref.write().await;
                        s.state = "no_disc".to_string();
                        toc_failed_since = None;
                    } else {
                        tracing::debug!("TOC no leído aún, reintentando en 3s (t+{}s)...", elapsed);
                    }
                }
            }

            last_disc_present = disc_present;
        }
    });
}
