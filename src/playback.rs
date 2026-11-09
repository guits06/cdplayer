use crate::cdda_ffi::{self, c_char_to_string, CDStatusC};
use crate::hardware::{detect_usb_dac, get_cd_device, set_drive_speed};
use crate::state::PlayerState;
use std::ffi::CString;
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::RwLock;

pub async fn stop_playback_unsafe(state_ref: Arc<RwLock<PlayerState>>) {
    let mut s = state_ref.write().await;
    s.session_id += 1;
    s.pc_audio_active = false;

    unsafe {
        cdda_ffi::cdda_stop();
    }

    if let Some(mut p) = s.fallback_p_read.take() {
        let _ = p.kill();
    }
    if let Some(mut p) = s.fallback_p_play.take() {
        let _ = p.kill();
    }
    if let Some(mut p) = s.pc_audio_proc.take() {
        let _ = p.kill();
    }

    s.bytes_played = 0;
    s.play_start_time = None;
    s.seek_offset_time = 0.0;

    if s.state == "playing"
        || s.state == "paused"
        || s.state == "pc_audio"
        || s.state == "starting"
    {
        s.state = "idle".to_string();
    }
}

pub async fn start_playback_unsafe(
    state_ref: Arc<RwLock<PlayerState>>,
    track: i32,
    seek_time: f64,
) {
    stop_playback_unsafe(state_ref.clone()).await;

    let (dev, dac_name, current_session, start_lba, total_sectors, buf_ms) = {
        let mut s = state_ref.write().await;
        s.session_id += 1;
        let session = s.session_id;

        let detected = detect_usb_dac(s.manual_dac, &s.dac_name);
        s.dac_name = detected.clone();
        if s.dac_name == "disconnected" {
            s.state = "idle".to_string();
            s.error_message =
                "DAC SMSL SU-1 no disponible (comprueba entrada USB en el DAC)".to_string();
            tracing::warn!("Intento de reproducción con DAC SMSL SU-1 desconectado.");
            return;
        }

        s.bytes_played = (seek_time * 176400.0) as u64;
        s.seek_offset_time = seek_time;
        s.play_start_time = Some(Instant::now());
        s.current_track = track;

        let dev = get_cd_device();

        let track_obj = s.tracks.iter().find(|t| t.track == track);
        let (s_lba, t_sec) = if let Some(t) = track_obj {
            let offset = (seek_time * 75.0) as i32;
            (t.start_lba + offset, (t.total_sectors - offset).max(75))
        } else {
            ((seek_time * 75.0) as i32, 75 * 300)
        };

        (dev, detected, session, s_lba, t_sec, s.buffer_ms)
    };

    set_drive_speed(2);

    // 1. Native C CDDA Audio Engine (Bit-Perfect, RAM ring buffer 8MB)
    let c_dev = CString::new(dev.clone()).unwrap_or_default();
    let alsa_dev_str = if dac_name != "default" {
        format!("plughw:{}", dac_name)
    } else {
        "default".to_string()
    };
    let c_alsa = CString::new(alsa_dev_str).unwrap_or_default();

    let ret = unsafe {
        cdda_ffi::cdda_set_device(c_dev.as_ptr());
        cdda_ffi::cdda_play(track, seek_time, c_alsa.as_ptr())
    };

    if ret == 0 {
        {
            let mut s = state_ref.write().await;
            s.state = "playing".to_string();
            s.current_track = track;
            s.error_message.clear();
        }
        let sr = state_ref.clone();
        tokio::spawn(async move {
            monitor_playback_native(sr, track, current_session).await;
        });
        return;
    } else if ret == -2 {
        let mut s = state_ref.write().await;
        s.tracks.clear();
        s.album_title.clear();
        s.album_artist.clear();
        s.cover_url.clear();
        s.state = "no_disc".to_string();
        s.error_message.clear();
        tracing::info!("No hay disco insertado en el lector (cdda_play=-2).");
        return;
    } else if ret == -3 {
        let mut s = state_ref.write().await;
        s.state = "idle".to_string();
        s.dac_name = "disconnected".to_string();
        s.error_message = "DAC SMSL SU-1 no disponible (modo consola)".to_string();
        tracing::warn!("DAC SMSL SU-1 no disponible en ALSA.");
        return;
    }

    // 2. Fallback pipe: cd-read -> aplay
    tracing::warn!("Motor nativo retornó código {}. Iniciando fallback cd-read -> aplay...", ret);
    let alsa_target = if dac_name != "default" {
        format!("plughw:{}", dac_name)
    } else {
        "default".to_string()
    };
    let buf_usec = buf_ms * 1000;

    let p_play = Command::new("aplay")
        .args([
            "-D",
            &alsa_target,
            "-t",
            "raw",
            "-f",
            "S16_LE",
            "-r",
            "44100",
            "-c",
            "2",
            "-B",
            &buf_usec.to_string(),
            "-F",
            "50000",
            "-q",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();

    match p_play {
        Ok(mut child_play) => {
            if let Some(stdin) = child_play.stdin.take() {
                let p_read = Command::new("cd-read")
                    .args([
                        "-C",
                        &dev,
                        "-m",
                        "audio",
                        "-s",
                        &start_lba.to_string(),
                        "-n",
                        &total_sectors.to_string(),
                        "--no-header",
                        "--no-hexdump",
                    ])
                    .stdout(stdin)
                    .stderr(Stdio::null())
                    .spawn();

                match p_read {
                    Ok(child_read) => {
                        let mut s = state_ref.write().await;
                        s.fallback_p_play = Some(child_play);
                        s.fallback_p_read = Some(child_read);
                        s.state = "playing".to_string();
                        s.current_track = track;
                        s.error_message.clear();
                    }
                    Err(e) => {
                        let mut s = state_ref.write().await;
                        s.state = "error".to_string();
                        s.error_message = format!("Error iniciando cd-read: {}", e);
                    }
                }
            }
        }
        Err(e) => {
            let mut s = state_ref.write().await;
            s.state = "error".to_string();
            s.error_message = format!("Error iniciando aplay: {}", e);
        }
    }
}

pub async fn monitor_playback_native(
    state_ref: Arc<RwLock<PlayerState>>,
    _track: i32,
    session_id: u64,
) {
    let mut st = CDStatusC::default();
    loop {
        tokio::time::sleep(Duration::from_millis(250)).await;

        let engine_state;
        {
            let mut s = state_ref.write().await;
            if s.session_id != session_id
                || (s.state != "playing" && s.state != "starting" && s.state != "paused")
            {
                break;
            }

            unsafe {
                cdda_ffi::cdda_get_status(&mut st);
            }
            s.current_track = st.current_track;
            s.bytes_played = (st.elapsed_time * 176400.0) as u64;
            s.buffer_fill_pct = st.buffer_fill_pct;
            engine_state = st.state;
        }

        // Disc finished naturally
        if engine_state == 0 {
            stop_playback_unsafe(state_ref.clone()).await;
            let mut s = state_ref.write().await;
            s.current_track = 1;
            s.state = "idle".to_string();
            s.error_message.clear();
            tracing::info!("Fin del disco alcanzado. Volviendo a modo espera (idle)...");
            break;
        }

        // Disc physically ejected during playback
        if engine_state == -2 {
            stop_playback_unsafe(state_ref.clone()).await;
            let mut s = state_ref.write().await;
            s.tracks.clear();
            s.album_title.clear();
            s.album_artist.clear();
            s.cover_url.clear();
            s.current_track = 1;
            s.state = "no_disc".to_string();
            s.error_message.clear();
            s.last_eject_time = Instant::now();
            tracing::info!("Disco expulsado físicamente del lector. Pasando a 'no_disc'.");
            break;
        }

        // DAC disconnected
        if engine_state == -3 {
            stop_playback_unsafe(state_ref.clone()).await;
            let mut s = state_ref.write().await;
            s.state = "idle".to_string();
            s.dac_name = "disconnected".to_string();
            let msg = c_char_to_string(&st.error_msg);
            s.error_message = if !msg.is_empty() {
                msg
            } else {
                "DAC SMSL SU-1 no disponible (modo consola)".to_string()
            };
            tracing::warn!("Estado DAC: {}", s.error_message);
            break;
        }

        // Unrecoverable engine error
        if engine_state == -1 {
            stop_playback_unsafe(state_ref.clone()).await;
            let mut s = state_ref.write().await;
            s.state = "error".to_string();
            let msg = c_char_to_string(&st.error_msg);
            s.error_message = if !msg.is_empty() {
                msg
            } else {
                "Fallo en motor de audio C".to_string()
            };
            tracing::error!("Error en motor nativo: {}", s.error_message);
            break;
        }
    }
}

pub async fn pause_playback(state_ref: Arc<RwLock<PlayerState>>) {
    let mut s = state_ref.write().await;
    if s.state == "playing" {
        unsafe {
            cdda_ffi::cdda_pause();
        }
        s.state = "paused".to_string();
        if let Some(start) = s.play_start_time {
            s.pause_position = (start.elapsed().as_secs_f64() + s.seek_offset_time).max(0.0);
        }
        if let Some(mut p) = s.fallback_p_play.take() {
            let _ = p.kill();
        }
    }
}

pub async fn resume_playback(state_ref: Arc<RwLock<PlayerState>>) {
    let (should_restart, cur_track, pos) = {
        let mut s = state_ref.write().await;
        if s.state == "paused" {
            unsafe {
                let r = cdda_ffi::cdda_resume();
                if r == 0 {
                    s.state = "playing".to_string();
                    s.play_start_time = Some(Instant::now());
                    return;
                }
            }
            (true, s.current_track, s.pause_position)
        } else {
            (false, 1, 0.0)
        }
    };

    if should_restart {
        start_playback_unsafe(state_ref, cur_track, pos).await;
    }
}
