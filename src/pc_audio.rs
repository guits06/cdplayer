use crate::hardware::{detect_usb_dac, sync_external_volume};
use crate::playback::stop_playback_unsafe;
use crate::state::PlayerState;
use std::net::UdpSocket;
use std::path::Path;
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::RwLock;

pub async fn start_pc_audio_receiver(mode: String, state_ref: Arc<RwLock<PlayerState>>) {
    stop_playback_unsafe(state_ref.clone()).await;

    let (dac_id, vol_l, vol_r, mute_l, mute_r) = {
        let mut s = state_ref.write().await;
        s.pc_audio_active = true;
        s.state = "pc_audio".to_string();
        s.error_message.clear();
        let detected = detect_usb_dac(s.manual_dac, &s.dac_name);
        s.dac_name = detected.clone();
        (detected, s.vol_l, s.vol_r, s.mute_l, s.mute_r)
    };

    let alsa_dev = if dac_id == "AUDIO" && Path::new("/etc/asound.conf").exists() {
        "stream_vol".to_string()
    } else if dac_id != "default" && dac_id != "disconnected" {
        format!("plughw:{}", dac_id)
    } else {
        "default".to_string()
    };

    sync_external_volume(vol_l, vol_r, mute_l, mute_r, &dac_id);

    tracing::info!(
        "Audio Receiver (GStreamer RTP) [Modo: {}]: Port 3000, DAC: {}",
        mode,
        alsa_dev
    );

    let (queue_params, alsa_params): (Vec<&str>, Vec<&str>) = match mode.as_str() {
        "ultra_low" => (
            vec![
                "queue",
                "max-size-buffers=3",
                "max-size-bytes=0",
                "max-size-time=15000000",
                "leaky=downstream",
            ],
            vec![
                "alsasink",
                "sync=false",
                "buffer-time=20000",
                "latency-time=5000",
            ],
        ),
        "lossless" => (
            vec![
                "queue",
                "max-size-buffers=0",
                "max-size-bytes=0",
                "max-size-time=120000000",
                "leaky=no",
            ],
            vec![
                "alsasink",
                "sync=false",
                "buffer-time=120000",
                "latency-time=20000",
            ],
        ),
        _ => (
            vec![
                "queue",
                "max-size-buffers=0",
                "max-size-bytes=0",
                "max-size-time=100000000",
                "leaky=no",
            ],
            vec![
                "alsasink",
                "sync=false",
                "buffer-time=100000",
                "latency-time=20000",
            ],
        ),
    };

    let mut args = vec![
        "-q",
        "udpsrc",
        "port=3000",
        "buffer-size=4194304",
        "caps=application/x-rtp,media=audio,clock-rate=48000,encoding-name=L24,channels=2",
        "!",
        "rtpL24depay",
        "!",
    ];
    args.extend(queue_params);
    args.extend(["!", "audioconvert", "!", "audioresample", "!"]);
    args.extend(alsa_params);
    let dev_arg = format!("device={}", alsa_dev);
    args.push(&dev_arg);

    match Command::new("gst-launch-1.0")
        .args(&args)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(child) => {
            let mut s = state_ref.write().await;
            s.pc_audio_proc = Some(child);
        }
        Err(e) => {
            tracing::error!("Error ejecutando gst-launch-1.0: {}", e);
            let mut s = state_ref.write().await;
            s.state = "error".to_string();
            s.error_message = format!("Error GStreamer: {}", e);
        }
    }

    // Spawn UDP ping responder
    tokio::spawn(async move {
        if let Ok(socket) = UdpSocket::bind("0.0.0.0:3001") {
            let _ = socket.set_read_timeout(Some(Duration::from_millis(500)));
            let mut buf = [0u8; 1024];
            while let Ok((amt, src)) = socket.recv_from(&mut buf) {
                let _ = socket.send_to(&buf[..amt], src);
            }
        }
    });
}

pub async fn stop_pc_audio_receiver(state_ref: Arc<RwLock<PlayerState>>) {
    let mut s = state_ref.write().await;
    s.pc_audio_active = false;
    if let Some(mut child) = s.pc_audio_proc.take() {
        let _ = child.kill();
    }
    if s.state == "pc_audio" {
        s.state = "idle".to_string();
    }
}
