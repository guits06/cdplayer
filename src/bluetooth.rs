use crate::hardware::{detect_usb_dac, sync_external_volume};
use crate::playback::stop_playback_unsafe;
use crate::state::PlayerState;
use std::collections::HashMap;
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::RwLock;
use zbus::names::{BusName, InterfaceName, MemberName};
use zbus::zvariant::{ObjectPath, Value};

pub const AUDIO_SOURCE_UUID: &str = "0000110a-0000-1000-8000-00805f9b34fb";
pub const HFP_AG_UUID: &str = "0000111f-0000-1000-8000-00805f9b34fb";

pub struct BluetoothController {
    state_ref: Arc<RwLock<PlayerState>>,
    aplay_proc: Arc<std::sync::Mutex<Option<Child>>>,
    autoplay_running: Arc<AtomicBool>,
}

fn val_to_str(val: &zbus::zvariant::OwnedValue) -> String {
    match &**val {
        Value::Str(s) => s.as_str().to_string(),
        Value::Value(inner) => match &**inner {
            Value::Str(s) => s.as_str().to_string(),
            _ => String::new(),
        },
        _ => String::new(),
    }
}

fn val_to_bool(val: &zbus::zvariant::OwnedValue) -> bool {
    match &**val {
        Value::Bool(b) => *b,
        Value::Value(inner) => match &**inner {
            Value::Bool(b) => *b,
            _ => false,
        },
        _ => false,
    }
}

fn val_to_u32(val: &zbus::zvariant::OwnedValue) -> u32 {
    match &**val {
        Value::U32(u) => *u,
        Value::U16(u) => *u as u32,
        Value::U8(u) => *u as u32,
        Value::I32(i) => *i as u32,
        Value::Value(inner) => match &**inner {
            Value::U32(u) => *u,
            Value::U16(u) => *u as u32,
            Value::U8(u) => *u as u32,
            Value::I32(i) => *i as u32,
            _ => 0,
        },
        _ => 0,
    }
}

async fn dbus_call<B: serde::Serialize + zbus::zvariant::DynamicType>(
    conn: &zbus::Connection,
    dest: &str,
    path: &str,
    iface: &str,
    method: &str,
    body: &B,
) -> zbus::Result<zbus::message::Message> {
    let p = ObjectPath::try_from(path).map_err(|e| zbus::Error::Failure(e.to_string()))?;
    let b = BusName::try_from(dest).map_err(|e| zbus::Error::Failure(e.to_string()))?;
    let i = InterfaceName::try_from(iface).map_err(|e| zbus::Error::Failure(e.to_string()))?;
    let m = MemberName::try_from(method).map_err(|e| zbus::Error::Failure(e.to_string()))?;
    conn.call_method(Some(b), p, Some(i), m, body).await
}

impl BluetoothController {
    pub fn new(state_ref: Arc<RwLock<PlayerState>>) -> Self {
        let aplay_proc = Arc::new(std::sync::Mutex::new(None));
        let autoplay_running = Arc::new(AtomicBool::new(false));

        let ctl = Self {
            state_ref: state_ref.clone(),
            aplay_proc: aplay_proc.clone(),
            autoplay_running: autoplay_running.clone(),
        };

        let sr = state_ref.clone();
        let ap = aplay_proc.clone();
        let ar = autoplay_running.clone();
        tokio::spawn(async move {
            monitor_loop(sr, ap, ar).await;
        });

        ctl
    }

    pub async fn set_mode(&self, mode: &str) {
        tracing::info!("[Bluetooth] set_mode called with: {}", mode);
        if mode == "off" {
            self.disable().await;
            return;
        }

        {
            let mut s = self.state_ref.write().await;
            s.pc_audio_active = false;
            if let Some(mut child) = s.pc_audio_proc.take() {
                let _ = child.kill();
            }

            if mode == "music" || mode == "both" {
                s.bluetooth_active = true;
                s.state = "bluetooth".to_string();
                s.bt_mode = mode.to_string();
                s.error_message.clear();
            } else if mode == "voip" {
                s.bt_mode = "voip".to_string();
                if s.state == "bluetooth" {
                    s.bluetooth_active = false;
                    s.state = "idle".to_string();
                }
            }
        }

        stop_playback_unsafe(self.state_ref.clone()).await;

        let _ = Command::new("bluetoothctl").args(["power", "on"]).output();

        set_adapter_properties(true, true).await;

        if mode == "music" || mode == "both" {
            self.start_aplay().await;
        } else {
            self.stop_aplay();
        }

        let mode_str = mode.to_string();
        let sr = self.state_ref.clone();
        let ar = self.autoplay_running.clone();
        tokio::spawn(async move {
            auto_connect_and_apply(&mode_str, sr, ar).await;
        });
    }

    pub async fn enable(&self, has_voip_capability: bool) {
        let mode = if has_voip_capability { "both" } else { "music" };
        self.set_mode(mode).await;
    }

    pub async fn disable(&self) {
        self.stop_aplay();
        set_adapter_properties(false, false).await;

        let _ = Command::new("bluetoothctl").arg("disconnect").spawn();
        let _ = Command::new("bluetoothctl").args(["power", "off"]).spawn();

        let _ = std::fs::remove_file("/tmp/current_cover.bmp");
        let _ = std::fs::remove_file("/tmp/current_cover.jpg");

        let mut s = self.state_ref.write().await;
        s.bluetooth_active = false;
        s.bt_mode = "off".to_string();
        s.bt_connected = false;
        s.bt_device_name.clear();
        s.bt_title.clear();
        s.bt_artist.clear();
        s.bt_album.clear();
        s.bt_status = "stopped".to_string();
        s.bt_duration = 0.0;
        s.bt_elapsed = 0.0;
        s.cover_url.clear();
        if s.state == "bluetooth" {
            s.state = "idle".to_string();
        }
    }

    pub async fn toggle(&self, has_voip_capability: bool) {
        let (is_active, cur_mode) = {
            let s = self.state_ref.read().await;
            (s.bluetooth_active, s.bt_mode.clone())
        };

        if cur_mode != "off" || is_active {
            self.disable().await;
        } else {
            let mode = if has_voip_capability { "both" } else { "music" };
            self.set_mode(mode).await;
        }
    }

    pub async fn start_aplay(&self) {
        self.stop_aplay();

        let (dac_id, vl, vr, ml, mr) = {
            let mut s = self.state_ref.write().await;
            let d = detect_usb_dac(s.manual_dac, &s.dac_name);
            s.dac_name = d.clone();
            (d, s.vol_l, s.vol_r, s.mute_l, s.mute_r)
        };

        let alsa_dev = if dac_id == "AUDIO" && Path::new("/etc/asound.conf").exists() {
            "bt_vol".to_string()
        } else if dac_id != "default" && dac_id != "disconnected" {
            format!("plughw:{}", dac_id)
        } else {
            "default".to_string()
        };

        match Command::new("bluealsa-aplay")
            .args(["-D", &alsa_dev, "--volume=none", "--profile-a2dp"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            Ok(child) => {
                tracing::info!(
                    "[Bluetooth] bluealsa-aplay started on {} (PID: {})",
                    alsa_dev,
                    child.id()
                );
                let mut lock = self.aplay_proc.lock().unwrap();
                *lock = Some(child);
            }
            Err(e) => {
                tracing::error!("[Bluetooth] Error starting bluealsa-aplay: {}", e);
            }
        }

        sync_external_volume(vl, vr, ml, mr, &dac_id);
    }

    pub fn stop_aplay(&self) {
        let mut lock = self.aplay_proc.lock().unwrap();
        if let Some(mut child) = lock.take() {
            let _ = child.kill();
            let _ = child.wait();
            tracing::info!("[Bluetooth] bluealsa-aplay stopped");
        }
    }

    pub async fn play(&self) {
        call_media_method("Play").await;
    }

    pub async fn pause(&self) {
        call_media_method("Pause").await;
    }

    pub async fn play_pause(&self) {
        let cur_status = {
            let s = self.state_ref.read().await;
            s.bt_status.clone()
        };
        if cur_status == "playing" {
            self.pause().await;
        } else {
            self.play().await;
        }
    }

    pub async fn next(&self) {
        call_media_method("Next").await;
    }

    pub async fn prev(&self) {
        call_media_method("Previous").await;
    }
}

async fn set_adapter_properties(discoverable: bool, pairable: bool) {
    if let Ok(conn) = zbus::Connection::system().await {
        let _ = dbus_call(
            &conn,
            "org.bluez",
            "/org/bluez/hci0",
            "org.freedesktop.DBus.Properties",
            "Set",
            &(
                "org.bluez.Adapter1",
                "Discoverable",
                Value::from(discoverable),
            ),
        )
        .await;

        let _ = dbus_call(
            &conn,
            "org.bluez",
            "/org/bluez/hci0",
            "org.freedesktop.DBus.Properties",
            "Set",
            &(
                "org.bluez.Adapter1",
                "Pairable",
                Value::from(pairable),
            ),
        )
        .await;
    }
}

async fn call_media_method(method: &str) {
    if let Ok(conn) = zbus::Connection::system().await {
        let (player_path, control_path) = find_player_paths(&conn).await;
        if let Some(path) = player_path {
            let _ = dbus_call(
                &conn,
                "org.bluez",
                &path,
                "org.bluez.MediaPlayer1",
                method,
                &(),
            )
            .await;
        } else if let Some(path) = control_path {
            let _ = dbus_call(
                &conn,
                "org.bluez",
                &path,
                "org.bluez.MediaControl1",
                method,
                &(),
            )
            .await;
        }
    }
}

async fn find_player_paths(
    conn: &zbus::Connection,
) -> (Option<String>, Option<String>) {
    let mut player = None;
    let mut control = None;

    if let Ok(reply) = dbus_call(
        conn,
        "org.bluez",
        "/",
        "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects",
        &(),
    )
    .await
    {
        if let Ok(body) = reply.body().deserialize::<HashMap<
            ObjectPath,
            HashMap<String, HashMap<String, zbus::zvariant::OwnedValue>>,
        >>() {
            for (path, ifaces) in body {
                let p_str = path.as_str().to_string();
                if ifaces.contains_key("org.bluez.MediaPlayer1") {
                    player = Some(p_str.clone());
                }
                if ifaces.contains_key("org.bluez.MediaControl1") {
                    control = Some(p_str);
                }
            }
        }
    }

    (player, control)
}

async fn auto_connect_and_apply(
    mode: &str,
    state_ref: Arc<RwLock<PlayerState>>,
    autoplay_running: Arc<AtomicBool>,
) {
    tokio::time::sleep(Duration::from_millis(400)).await;

    if let Ok(conn) = zbus::Connection::system().await {
        let reply = dbus_call(
            &conn,
            "org.bluez",
            "/",
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects",
            &(),
        )
        .await;

        let mut candidate_path: Option<String> = None;
        let mut candidate_name = String::new();

        if let Ok(msg) = reply {
            if let Ok(objects) = msg.body().deserialize::<HashMap<
                ObjectPath,
                HashMap<String, HashMap<String, zbus::zvariant::OwnedValue>>,
            >>() {
                for (path, ifaces) in &objects {
                    if let Some(dev) = ifaces.get("org.bluez.Device1") {
                        let paired = dev.get("Paired").map(val_to_bool).unwrap_or(false);
                        let icon = dev.get("Icon").map(val_to_str).unwrap_or_default();
                        let addr = dev.get("Address").map(val_to_str).unwrap_or_default();
                        let alias = dev.get("Alias").map(val_to_str).unwrap_or_default();

                        if paired && (icon == "phone" || addr == "24:95:2F:5C:4E:D3") {
                            candidate_path = Some(path.as_str().to_string());
                            candidate_name = alias;
                            break;
                        }
                    }
                }
            }
        }

        if let Some(path) = candidate_path {
            tracing::info!(
                "[Bluetooth] Connecting {} ({}) for mode '{}'...",
                candidate_name,
                path,
                mode
            );

            let init_uuid = if mode == "voip" {
                HFP_AG_UUID
            } else {
                AUDIO_SOURCE_UUID
            };

            let _ = dbus_call(
                &conn,
                "org.bluez",
                &path,
                "org.bluez.Device1",
                "ConnectProfile",
                &(init_uuid,),
            )
            .await;

            tokio::time::sleep(Duration::from_millis(300)).await;

            if mode == "voip" {
                let _ = dbus_call(
                    &conn,
                    "org.bluez",
                    &path,
                    "org.bluez.Device1",
                    "ConnectProfile",
                    &(HFP_AG_UUID,),
                )
                .await;
                let _ = dbus_call(
                    &conn,
                    "org.bluez",
                    &path,
                    "org.bluez.Device1",
                    "DisconnectProfile",
                    &(AUDIO_SOURCE_UUID,),
                )
                .await;
            } else if mode == "music" {
                let _ = dbus_call(
                    &conn,
                    "org.bluez",
                    &path,
                    "org.bluez.Device1",
                    "ConnectProfile",
                    &(AUDIO_SOURCE_UUID,),
                )
                .await;
                let _ = dbus_call(
                    &conn,
                    "org.bluez",
                    &path,
                    "org.bluez.Device1",
                    "DisconnectProfile",
                    &(HFP_AG_UUID,),
                )
                .await;
                trigger_autoplay(state_ref, autoplay_running);
            } else if mode == "both" {
                let _ = dbus_call(
                    &conn,
                    "org.bluez",
                    &path,
                    "org.bluez.Device1",
                    "ConnectProfile",
                    &(HFP_AG_UUID,),
                )
                .await;
                let _ = dbus_call(
                    &conn,
                    "org.bluez",
                    &path,
                    "org.bluez.Device1",
                    "ConnectProfile",
                    &(AUDIO_SOURCE_UUID,),
                )
                .await;
                trigger_autoplay(state_ref, autoplay_running);
            }
        }
    }
}

fn trigger_autoplay(state_ref: Arc<RwLock<PlayerState>>, autoplay_running: Arc<AtomicBool>) {
    if autoplay_running
        .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
        .is_err()
    {
        return;
    }

    tokio::spawn(async move {
        for attempt in 0..20 {
            tokio::time::sleep(Duration::from_millis(500)).await;
            {
                let s = state_ref.read().await;
                if !s.bluetooth_active || !s.bt_connected {
                    break;
                }
                if s.bt_status == "playing" {
                    tracing::info!(
                        "[Bluetooth] Auto-play active (status is playing on attempt {})",
                        attempt + 1
                    );
                    break;
                }
            }
            call_media_method("Play").await;
        }
        autoplay_running.store(false, Ordering::SeqCst);
    });
}

async fn monitor_loop(
    state_ref: Arc<RwLock<PlayerState>>,
    aplay_proc: Arc<std::sync::Mutex<Option<Child>>>,
    autoplay_running: Arc<AtomicBool>,
) {
    let mut was_connected = false;
    let mut last_art_url = String::new();

    loop {
        tokio::time::sleep(Duration::from_millis(500)).await;

        let cur_mode = {
            let s = state_ref.read().await;
            s.bt_mode.clone()
        };

        if let Ok(conn) = zbus::Connection::system().await {
            let reply = dbus_call(
                &conn,
                "org.bluez",
                "/",
                "org.freedesktop.DBus.ObjectManager",
                "GetManagedObjects",
                &(),
            )
            .await;

            if let Ok(msg) = reply {
                if let Ok(objects) = msg.body().deserialize::<HashMap<
                    ObjectPath,
                    HashMap<String, HashMap<String, zbus::zvariant::OwnedValue>>,
                >>() {
                    let mut found_connected_dev = false;
                    let mut dev_name = String::new();
                    let mut player_path: Option<String> = None;

                    for (path, ifaces) in &objects {
                        if let Some(dev) = ifaces.get("org.bluez.Device1") {
                            let connected = dev.get("Connected").map(val_to_bool).unwrap_or(false);
                            if connected {
                                found_connected_dev = true;
                                dev_name = dev
                                    .get("Alias")
                                    .or_else(|| dev.get("Name"))
                                    .map(val_to_str)
                                    .unwrap_or_default();
                            }
                        }
                        if ifaces.contains_key("org.bluez.MediaPlayer1") {
                            player_path = Some(path.as_str().to_string());
                        }
                    }

                    let just_connected = found_connected_dev && !was_connected;
                    was_connected = found_connected_dev;

                    {
                        let mut s = state_ref.write().await;
                        s.bt_connected = found_connected_dev;
                        if found_connected_dev {
                            s.bt_device_name = dev_name.clone();
                        }
                    }

                    if just_connected && (cur_mode == "music" || cur_mode == "both") {
                        tracing::info!(
                            "[Bluetooth] Device connected: {}. Triggering auto-play...",
                            dev_name
                        );
                        trigger_autoplay(state_ref.clone(), autoplay_running.clone());
                    }

                    if found_connected_dev
                        && player_path.is_some()
                        && (cur_mode == "music" || cur_mode == "both")
                    {
                        let p_str = player_path.unwrap();
                        let props_reply = dbus_call(
                            &conn,
                            "org.bluez",
                            &p_str,
                            "org.freedesktop.DBus.Properties",
                            "GetAll",
                            &("org.bluez.MediaPlayer1",),
                        )
                        .await;

                        if let Ok(p_msg) = props_reply {
                            if let Ok(props) = p_msg
                                .body()
                                .deserialize::<HashMap<String, zbus::zvariant::OwnedValue>>()
                            {
                                let status = props
                                    .get("Status")
                                    .map(val_to_str)
                                    .unwrap_or_else(|| "stopped".to_string())
                                    .to_lowercase();

                                let pos_ms = props.get("Position").map(val_to_u32).unwrap_or(0);

                                let mut title = String::new();
                                let mut artist = String::new();
                                let mut album = String::new();
                                let mut dur_ms = 0u32;
                                let mut art_url = String::new();

                                if let Some(track_val) = props.get("Track") {
                                    if let Value::Dict(dict) = &**track_val {
                                        for (k, v) in dict.iter() {
                                            if let (Value::Str(key), Value::Value(inner)) = (k, v) {
                                                match key.as_str() {
                                                    "Title" => {
                                                        if let Value::Str(s) = &**inner {
                                                            title = s.as_str().to_string();
                                                        }
                                                    }
                                                    "Artist" => {
                                                        if let Value::Str(s) = &**inner {
                                                            artist = s.as_str().to_string();
                                                        }
                                                    }
                                                    "Album" => {
                                                        if let Value::Str(s) = &**inner {
                                                            album = s.as_str().to_string();
                                                        }
                                                    }
                                                    "Duration" => {
                                                        if let Value::U32(d) = &**inner {
                                                            dur_ms = *d;
                                                        }
                                                    }
                                                    "mpris:artUrl" | "ArtUrl" => {
                                                        if let Value::Str(s) = &**inner {
                                                            art_url = s.as_str().to_string();
                                                        }
                                                    }
                                                    _ => {}
                                                }
                                            }
                                        }
                                    }
                                }

                                {
                                    let mut s = state_ref.write().await;
                                    s.bt_status = status;
                                    s.bt_title = title;
                                    s.bt_artist = artist;
                                    s.bt_album = album;
                                    s.bt_duration = (dur_ms as f64) / 1000.0;
                                    s.bt_elapsed = (pos_ms as f64) / 1000.0;
                                }

                                if !art_url.is_empty() && art_url != last_art_url {
                                    last_art_url = art_url.clone();
                                    process_cover_art(&art_url, state_ref.clone()).await;
                                }
                            }
                        }
                    } else if !found_connected_dev {
                        last_art_url.clear();
                        let _ = std::fs::remove_file("/tmp/current_cover.bmp");
                        let _ = std::fs::remove_file("/tmp/current_cover.jpg");
                        let mut s = state_ref.write().await;
                        s.bt_status = "stopped".to_string();
                        s.bt_title.clear();
                        s.bt_artist.clear();
                        s.bt_album.clear();
                        s.bt_duration = 0.0;
                        s.bt_elapsed = 0.0;
                        s.cover_url.clear();
                    }

                    // Check if bluealsa-aplay is alive
                    if cur_mode == "music" || cur_mode == "both" {
                        let is_dead = {
                            let mut lock = aplay_proc.lock().unwrap();
                            if let Some(child) = lock.as_mut() {
                                child.try_wait().map(|s| s.is_some()).unwrap_or(true)
                            } else {
                                true
                            }
                        };
                        if is_dead {
                            let dac_id = {
                                let s = state_ref.read().await;
                                s.dac_name.clone()
                            };
                            let alsa_dev =
                                if dac_id == "AUDIO" && Path::new("/etc/asound.conf").exists() {
                                    "bt_vol".to_string()
                                } else if dac_id != "default" && dac_id != "disconnected" {
                                    format!("plughw:{}", dac_id)
                                } else {
                                    "default".to_string()
                                };

                            if let Ok(child) = Command::new("bluealsa-aplay")
                                .args(["-D", &alsa_dev, "--volume=none", "--profile-a2dp"])
                                .stdout(Stdio::null())
                                .stderr(Stdio::null())
                                .spawn()
                            {
                                let mut lock = aplay_proc.lock().unwrap();
                                *lock = Some(child);
                            }
                        }
                    }
                }
            }
        }
    }
}

async fn process_cover_art(art_url: &str, state_ref: Arc<RwLock<PlayerState>>) {
    let local_path = if let Some(path) = art_url.strip_prefix("file://") {
        urlencoding_decode(path)
    } else {
        art_url.to_string()
    };

    if Path::new(&local_path).exists() {
        let _ = Command::new("ffmpeg")
            .args([
                "-y",
                "-i",
                &local_path,
                "-vf",
                "scale=240:240",
                "/tmp/current_cover.bmp",
            ])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();

        let mut s = state_ref.write().await;
        s.cover_url = art_url.to_string();
    }
}

fn urlencoding_decode(s: &str) -> String {
    let mut out = Vec::new();
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let Ok(h) =
                u8::from_str_radix(std::str::from_utf8(&bytes[i + 1..i + 3]).unwrap_or(""), 16)
            {
                out.push(h);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).to_string()
}
