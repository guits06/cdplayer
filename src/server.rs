use crate::bluetooth::BluetoothController;
use crate::cd_monitor::get_track_list;
use crate::hardware::{detect_usb_dac, get_audio_interfaces, get_cd_device, sync_external_volume};
use crate::modules::ModuleManager;
use crate::musicbrainz::query_musicbrainz;
use crate::playback::{pause_playback, resume_playback, start_playback_unsafe, stop_playback_unsafe};
use crate::pc_audio::{start_pc_audio_receiver, stop_pc_audio_receiver};
use crate::state::PlayerState;
use axum::{
    extract::{Path as AxumPath, Request, State},
    http::{header, HeaderValue, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, options, post},
    Router,
};
use futures::StreamExt;
use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::sync::Arc;
use tokio::sync::RwLock;

#[derive(Clone)]
pub struct AppContext {
    pub state: Arc<RwLock<PlayerState>>,
    pub modules: Arc<ModuleManager>,
    pub bluetooth: Arc<BluetoothController>,
    pub default_file: &'static str,
}

pub fn create_router(ctx: AppContext) -> Router {
    Router::new()
        // API GET
        .route("/api/status", get(handle_get_status))
        .route("/api/interfaces", get(handle_get_interfaces))
        .route("/api/volume", get(handle_get_volume))
        .route("/api/modules", get(handle_get_modules))
        .route("/api/modules/:id/ui", get(handle_get_module_ui))
        .route("/api/tracks", get(handle_get_tracks))
        .route("/api/play", get(handle_get_play))
        .route("/api/pause", get(handle_get_pause))
        .route("/api/resume", get(handle_get_resume))
        .route("/api/stop", get(handle_get_stop))
        .route("/api/lookup", get(handle_get_lookup))
        .route("/api/study/status", get(handle_study_status))
        .route("/api/study/backgrounds", get(handle_study_backgrounds))
        // API POST
        .route("/api/speedtest", post(handle_post_speedtest))
        .route("/api/volume/reset", post(handle_post_volume_reset))
        .route("/api/volume/mute", post(handle_post_volume_mute))
        .route("/api/volume", post(handle_post_volume))
        .route("/api/play", post(handle_post_play))
        .route("/api/pause", post(handle_post_pause))
        .route("/api/resume", post(handle_post_resume))
        .route("/api/stop", post(handle_post_stop))
        .route("/api/prev", post(handle_post_prev))
        .route("/api/next", post(handle_post_next))
        .route("/api/seek", post(handle_post_seek))
        .route("/api/select_interface", post(handle_post_select_interface))
        .route("/api/pc_audio/enable", post(handle_post_pc_audio_enable))
        .route("/api/pc_audio/disable", post(handle_post_pc_audio_disable))
        .route("/api/modules/toggle", post(handle_post_modules_toggle))
        .route("/api/modules/:id/toggle", post(handle_post_module_id_toggle))
        .route("/api/modules/voip", post(handle_post_modules_voip))
        .route("/api/bluetooth/mode", post(handle_post_bluetooth_mode))
        .route("/api/bluetooth/enable", post(handle_post_bluetooth_enable))
        .route("/api/bluetooth/disable", post(handle_post_bluetooth_disable))
        .route("/api/bluetooth/toggle", post(handle_post_bluetooth_toggle))
        .route("/api/bluetooth/play_pause", post(handle_post_bluetooth_play_pause))
        .route("/api/bluetooth/play", post(handle_post_bluetooth_play))
        .route("/api/bluetooth/pause", post(handle_post_bluetooth_pause))
        .route("/api/bluetooth/next", post(handle_post_bluetooth_next))
        .route("/api/bluetooth/prev", post(handle_post_bluetooth_prev))
        .route("/api/eject", post(handle_post_eject))
        .route("/api/load", post(handle_post_load))
        .route("/api/refresh", post(handle_post_refresh))
        .route("/api/set_buffer", post(handle_post_set_buffer))
        .route("/api/set_release", post(handle_post_set_release))
        .route("/api/study/enable", post(handle_study_enable))
        .route("/api/study/disable", post(handle_study_disable))
        .route("/api/study/lofi/start", post(handle_study_lofi_start))
        .route("/api/study/lofi/stop", post(handle_study_lofi_stop))
        .route("/api/study/whitenoise/start", post(handle_study_whitenoise_start))
        .route("/api/study/whitenoise/stop", post(handle_study_whitenoise_stop))
        .route("/api/study/volume", post(handle_study_volume))
        // OPTIONS fallback for CORS
        .route("/*path", options(handle_options))
        // Fallback for static files
        .fallback(handle_static_fallback)
        .with_state(ctx)
}

fn json_response<T: serde::Serialize>(data: &T, status: StatusCode) -> Response {
    match serde_json::to_vec(data) {
        Ok(bytes) => {
            let mut res = (status, bytes).into_response();
            let headers = res.headers_mut();
            headers.insert(header::CONTENT_TYPE, HeaderValue::from_static("application/json"));
            headers.insert(header::ACCESS_CONTROL_ALLOW_ORIGIN, HeaderValue::from_static("*"));
            headers.insert(
                header::CACHE_CONTROL,
                HeaderValue::from_static("no-cache, no-store, must-revalidate"),
            );
            headers.insert(header::PRAGMA, HeaderValue::from_static("no-cache"));
            headers.insert(header::EXPIRES, HeaderValue::from_static("0"));
            res
        }
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()).into_response(),
    }
}

async fn get_snapshot(ctx: &AppContext) -> Response {
    let mods = ctx.modules.list_modules(false).await;
    let caps = ctx.modules.get_active_capabilities().await;
    let has_voip = ctx.modules.has_capability("telephony").await;
    let has_study = ctx.modules.has_capability("study").await;

    let snap = {
        let mut s = ctx.state.write().await;
        if s.state == "idle" {
            let d = detect_usb_dac(s.manual_dac, &s.dac_name);
            s.dac_name = d;
        }
        s.snapshot(mods, caps, has_voip, has_study)
    };

    json_response(&snap, StatusCode::OK)
}

async fn handle_options() -> Response {
    let mut res = StatusCode::OK.into_response();
    let headers = res.headers_mut();
    headers.insert(header::ACCESS_CONTROL_ALLOW_ORIGIN, HeaderValue::from_static("*"));
    headers.insert(
        header::ACCESS_CONTROL_ALLOW_METHODS,
        HeaderValue::from_static("GET, POST, OPTIONS"),
    );
    headers.insert(
        header::ACCESS_CONTROL_ALLOW_HEADERS,
        HeaderValue::from_static("Content-Type"),
    );
    res
}

async fn parse_params(req: Request) -> HashMap<String, serde_json::Value> {
    let mut map = HashMap::new();
    if let Some(query) = req.uri().query() {
        for (k, v) in url::form_urlencoded::parse(query.as_bytes()) {
            map.insert(k.into_owned(), serde_json::Value::String(v.into_owned()));
        }
    }

    let bytes = axum::body::to_bytes(req.into_body(), 10 * 1024 * 1024)
        .await
        .unwrap_or_default();
    if !bytes.is_empty() {
        if let Ok(serde_json::Value::Object(obj)) = serde_json::from_slice(&bytes) {
            for (k, v) in obj {
                map.insert(k, v);
            }
        }
    }
    map
}

// --- GET Handlers ---

async fn handle_get_status(State(ctx): State<AppContext>) -> Response {
    get_snapshot(&ctx).await
}

async fn handle_get_interfaces() -> Response {
    let ifaces = get_audio_interfaces();
    json_response(&ifaces, StatusCode::OK)
}

async fn handle_get_volume(State(ctx): State<AppContext>) -> Response {
    let s = ctx.state.read().await;
    let bp = s.vol_l >= 100 && s.vol_r >= 100 && !s.mute_l && !s.mute_r;
    let res = serde_json::json!({
        "vol_l": s.vol_l,
        "vol_r": s.vol_r,
        "mute_l": s.mute_l,
        "mute_r": s.mute_r,
        "bit_perfect": bp
    });
    json_response(&res, StatusCode::OK)
}

async fn handle_get_modules(State(ctx): State<AppContext>) -> Response {
    let mods = ctx.modules.list_modules(true).await;
    let caps = ctx.modules.get_active_capabilities().await;
    let has_voip = ctx.modules.has_capability("telephony").await;
    let has_study = ctx.modules.has_capability("study").await;

    let res = serde_json::json!({
        "modules": mods,
        "capabilities": caps,
        "voip_enabled": has_voip,
        "study_enabled": has_study
    });
    json_response(&res, StatusCode::OK)
}

async fn handle_get_module_ui(
    State(ctx): State<AppContext>,
    AxumPath(id): AxumPath<String>,
) -> Response {
    if let Some(ui_data) = ctx.modules.get_module_ui(&id).await {
        json_response(&ui_data, StatusCode::OK)
    } else {
        json_response(
            &serde_json::json!({"error": "No UI available for module"}),
            StatusCode::NOT_FOUND,
        )
    }
}

async fn handle_get_tracks(State(ctx): State<AppContext>) -> Response {
    let (has_tracks, cur_state) = {
        let s = ctx.state.read().await;
        (!s.tracks.is_empty(), s.state.clone())
    };

    if !has_tracks && cur_state != "no_disc" {
        let sr1 = ctx.state.clone();
        let sr2 = ctx.state.clone();
        tokio::spawn(async move {
            let res = tokio::task::spawn_blocking(move || {
                get_track_list(Some(sr1))
            }).await;
            if let Ok((t, _, _, _, _)) = res {
                if !t.is_empty() {
                    let mut s = sr2.write().await;
                    s.tracks = t;
                    s.state = "idle".to_string();
                }
            }
        });
    }

    get_snapshot(&ctx).await
}

async fn handle_get_play(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let target_track = params
        .get("track")
        .and_then(|v| v.as_i64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
        .map(|v| v as i32);
    let seek_time = params
        .get("seek")
        .or_else(|| params.get("position"))
        .and_then(|v| v.as_f64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
        .unwrap_or(0.0);

    let tr = match target_track {
        Some(t) => t,
        None => {
            let s = ctx.state.read().await;
            s.current_track
        }
    };

    start_playback_unsafe(ctx.state.clone(), tr, seek_time).await;
    get_snapshot(&ctx).await
}

async fn handle_get_pause(State(ctx): State<AppContext>) -> Response {
    pause_playback(ctx.state.clone()).await;
    get_snapshot(&ctx).await
}

async fn handle_get_resume(State(ctx): State<AppContext>) -> Response {
    resume_playback(ctx.state.clone()).await;
    get_snapshot(&ctx).await
}

async fn handle_get_stop(State(ctx): State<AppContext>) -> Response {
    stop_playback_unsafe(ctx.state.clone()).await;
    get_snapshot(&ctx).await
}

async fn handle_get_lookup(State(ctx): State<AppContext>) -> Response {
    let (offsets, leadout, actual_durations) = {
        let s = ctx.state.read().await;
        let mut actual = HashMap::new();
        for t in &s.tracks {
            actual.insert(t.track, t.duration);
        }
        (s.toc_offsets.clone(), s.toc_leadout, actual)
    };

    let res = query_musicbrainz(&offsets, leadout, &actual_durations).await;
    json_response(&res, StatusCode::OK)
}

async fn handle_study_status(State(ctx): State<AppContext>) -> Response {
    let s = ctx.state.read().await;
    let res = serde_json::json!({
        "study_mode_active": s.study_mode_active,
        "study_lofi_active": s.study_lofi_active,
        "study_whitenoise_active": s.study_whitenoise_active,
        "aux_volume": s.aux_volume,
    });
    json_response(&res, StatusCode::OK)
}

async fn handle_study_backgrounds(State(ctx): State<AppContext>) -> Response {
    let bg_dir = "/home/guille/cdplayer/study_backgrounds";
    let mut bgs = Vec::new();
    if let Ok(entries) = std::fs::read_dir(bg_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            let lower = name.to_lowercase();
            if lower.ends_with(".jpg") || lower.ends_with(".jpeg") || lower.ends_with(".png") {
                bgs.push(name);
            }
        }
    }
    bgs.sort();
    let cur = {
        let s = ctx.state.read().await;
        s.study_bg_current.clone()
    };
    let res = serde_json::json!({
        "backgrounds": bgs,
        "current": cur
    });
    json_response(&res, StatusCode::OK)
}

// --- POST Handlers ---

async fn handle_post_speedtest(req: Request) -> Response {
    let mut body = req.into_body().into_data_stream();
    let mut bytes_received = 0usize;
    while let Some(chunk_res) = body.next().await {
        if let Ok(chunk) = chunk_res {
            bytes_received += chunk.len();
        }
    }
    let res = serde_json::json!({
        "speed_test": "ok",
        "bytes_received": bytes_received
    });
    json_response(&res, StatusCode::OK)
}

async fn handle_post_volume_reset(State(ctx): State<AppContext>) -> Response {
    let dac = {
        let mut s = ctx.state.write().await;
        s.vol_l = 100;
        s.vol_r = 100;
        s.mute_l = false;
        s.mute_r = false;
        s.dac_name.clone()
    };
    sync_external_volume(100, 100, false, false, &dac);
    get_snapshot(&ctx).await
}

async fn handle_post_volume_mute(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let ch = params
        .get("channel")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_uppercase();

    let (vl, vr, ml, mr, dac) = {
        let mut s = ctx.state.write().await;
        if ch == "L" {
            s.mute_l = !s.mute_l;
        } else if ch == "R" {
            s.mute_r = !s.mute_r;
        } else if ch == "BOTH" || ch == "ALL" {
            let new_m = !(s.mute_l && s.mute_r);
            s.mute_l = new_m;
            s.mute_r = new_m;
        }
        if let Some(m_l) = params.get("mute_l").and_then(|v| v.as_bool()) {
            s.mute_l = m_l;
        }
        if let Some(m_r) = params.get("mute_r").and_then(|v| v.as_bool()) {
            s.mute_r = m_r;
        }
        (s.vol_l, s.vol_r, s.mute_l, s.mute_r, s.dac_name.clone())
    };

    sync_external_volume(vl, vr, ml, mr, &dac);
    get_snapshot(&ctx).await
}

async fn handle_post_volume(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;

    let (vl, vr, ml, mr, dac) = {
        let mut s = ctx.state.write().await;
        if params.get("reset").and_then(|v| v.as_bool()).unwrap_or(false) {
            s.vol_l = 100;
            s.vol_r = 100;
            s.mute_l = false;
            s.mute_r = false;
        } else {
            if let Some(v_l) = params
                .get("vol_l")
                .and_then(|v| v.as_i64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            {
                s.vol_l = (v_l as i32).clamp(0, 100);
            }
            if let Some(v_r) = params
                .get("vol_r")
                .and_then(|v| v.as_i64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            {
                s.vol_r = (v_r as i32).clamp(0, 100);
            }
            if let Some(m_l) = params.get("mute_l").and_then(|v| v.as_bool()) {
                s.mute_l = m_l;
            }
            if let Some(m_r) = params.get("mute_r").and_then(|v| v.as_bool()) {
                s.mute_r = m_r;
            }
        }
        (s.vol_l, s.vol_r, s.mute_l, s.mute_r, s.dac_name.clone())
    };

    sync_external_volume(vl, vr, ml, mr, &dac);
    get_snapshot(&ctx).await
}

async fn handle_post_play(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let target_track = params
        .get("track")
        .and_then(|v| v.as_i64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
        .map(|v| v as i32);
    let seek_time = params
        .get("seek")
        .or_else(|| params.get("position"))
        .and_then(|v| v.as_f64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
        .unwrap_or(0.0);

    let mut should_start = false;
    let mut track_to_play = 1;
    let mut actual_seek = seek_time;

    {
        let mut s = ctx.state.write().await;
        if s.state == "paused" && target_track.is_none() && seek_time == 0.0 {
            unsafe {
                let r = crate::cdda_ffi::cdda_resume();
                if r == 0 {
                    s.state = "playing".to_string();
                } else {
                    should_start = true;
                    track_to_play = s.current_track;
                    actual_seek = s.pause_position;
                }
            }
        } else if s.state == "playing" && target_track.is_none() && seek_time == 0.0 {
            // Already playing
        } else {
            should_start = true;
            track_to_play = target_track.unwrap_or(s.current_track);
        }
    }

    if should_start {
        start_playback_unsafe(ctx.state.clone(), track_to_play, actual_seek).await;
    }

    get_snapshot(&ctx).await
}

async fn handle_post_pause(State(ctx): State<AppContext>) -> Response {
    let is_bt = { ctx.state.read().await.bluetooth_active };
    if is_bt {
        ctx.bluetooth.pause().await;
    } else {
        pause_playback(ctx.state.clone()).await;
    }
    get_snapshot(&ctx).await
}

async fn handle_post_resume(State(ctx): State<AppContext>) -> Response {
    let is_bt = { ctx.state.read().await.bluetooth_active };
    if is_bt {
        ctx.bluetooth.play().await;
    } else {
        resume_playback(ctx.state.clone()).await;
    }
    get_snapshot(&ctx).await
}

async fn handle_post_stop(State(ctx): State<AppContext>) -> Response {
    let is_bt = { ctx.state.read().await.bluetooth_active };
    if is_bt {
        ctx.bluetooth.disable().await;
    } else {
        stop_playback_unsafe(ctx.state.clone()).await;
    }
    get_snapshot(&ctx).await
}

async fn handle_post_prev(State(ctx): State<AppContext>) -> Response {
    let is_bt = { ctx.state.read().await.bluetooth_active };
    if is_bt {
        ctx.bluetooth.prev().await;
    } else {
        let (target, _elapsed) = {
            let s = ctx.state.read().await;
            let elapsed = if let Some(start) = s.play_start_time {
                start.elapsed().as_secs_f64() + s.seek_offset_time
            } else {
                (s.bytes_played as f64) / 176400.0
            };
            let cur = s.current_track;
            let target = if elapsed > 3.0 || cur <= 1 {
                cur
            } else {
                let pt = cur - 1;
                if s.tracks.iter().any(|t| t.track == pt) {
                    pt
                } else {
                    cur
                }
            };
            (target, elapsed)
        };
        start_playback_unsafe(ctx.state.clone(), target, 0.0).await;
    }
    get_snapshot(&ctx).await
}

async fn handle_post_next(State(ctx): State<AppContext>) -> Response {
    let is_bt = { ctx.state.read().await.bluetooth_active };
    if is_bt {
        ctx.bluetooth.next().await;
    } else {
        let target = {
            let s = ctx.state.read().await;
            let nt = s.current_track + 1;
            if s.tracks.iter().any(|t| t.track == nt) {
                Some(nt)
            } else {
                None
            }
        };
        if let Some(nt) = target {
            start_playback_unsafe(ctx.state.clone(), nt, 0.0).await;
        }
    }
    get_snapshot(&ctx).await
}

async fn handle_post_seek(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let pos = params
        .get("position")
        .or_else(|| params.get("seconds"))
        .and_then(|v| v.as_f64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
        .unwrap_or(0.0);

    let cur_track = { ctx.state.read().await.current_track };
    start_playback_unsafe(ctx.state.clone(), cur_track, pos).await;
    get_snapshot(&ctx).await
}

async fn handle_post_select_interface(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let dac_id = params
        .get("dac_id")
        .and_then(|v| v.as_str())
        .unwrap_or("default")
        .to_string();

    let (was_playing, cur_track, curr_pos, is_pc) = {
        let mut s = ctx.state.write().await;
        s.dac_name = dac_id.clone();
        s.manual_dac = dac_id != "default";
        let is_p = s.state == "playing";
        let pos = (s.bytes_played as f64) / 176400.0;
        (is_p, s.current_track, pos, s.pc_audio_active)
    };

    if was_playing {
        start_playback_unsafe(ctx.state.clone(), cur_track, curr_pos).await;
    } else if is_pc {
        start_pc_audio_receiver("low".to_string(), ctx.state.clone()).await;
    }

    get_snapshot(&ctx).await
}

async fn handle_post_pc_audio_enable(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let mode = params
        .get("mode")
        .and_then(|v| v.as_str())
        .unwrap_or("low")
        .to_string();

    ctx.bluetooth.disable().await;
    start_pc_audio_receiver(mode, ctx.state.clone()).await;
    get_snapshot(&ctx).await
}

async fn handle_post_pc_audio_disable(State(ctx): State<AppContext>) -> Response {
    stop_pc_audio_receiver(ctx.state.clone()).await;
    get_snapshot(&ctx).await
}

async fn handle_post_modules_toggle(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let mod_id = params
        .get("id")
        .and_then(|v| v.as_str())
        .unwrap_or_default();
    let enable_val = params.get("enabled").and_then(|v| v.as_bool());

    let (ok, target_state) = ctx.modules.toggle_module(mod_id, enable_val).await;
    let res = serde_json::json!({
        "status": if ok { "ok" } else { "error" },
        "id": mod_id,
        "enabled": target_state
    });
    json_response(&res, StatusCode::OK)
}

async fn handle_post_module_id_toggle(
    State(ctx): State<AppContext>,
    AxumPath(id): AxumPath<String>,
    req: Request,
) -> Response {
    let params = parse_params(req).await;
    let enable_val = params.get("enabled").and_then(|v| v.as_bool());

    let (ok, target_state) = ctx.modules.toggle_module(&id, enable_val).await;
    let res = serde_json::json!({
        "status": if ok { "ok" } else { "error" },
        "id": id,
        "enabled": target_state
    });
    json_response(&res, StatusCode::OK)
}

async fn handle_post_modules_voip(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let enable_val = params.get("enabled").and_then(|v| v.as_bool());

    let (ok, target_state) = ctx.modules.toggle_module("cisco_voip", enable_val).await;
    let res = serde_json::json!({
        "status": if ok { "ok" } else { "error" },
        "voip_enabled": target_state
    });
    json_response(&res, StatusCode::OK)
}

async fn handle_post_bluetooth_mode(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let mode = params
        .get("mode")
        .and_then(|v| v.as_str())
        .unwrap_or("both");
    ctx.bluetooth.set_mode(mode).await;
    get_snapshot(&ctx).await
}

async fn handle_post_bluetooth_enable(State(ctx): State<AppContext>) -> Response {
    let has_voip = ctx.modules.has_capability("telephony").await;
    ctx.bluetooth.enable(has_voip).await;
    get_snapshot(&ctx).await
}

async fn handle_post_bluetooth_disable(State(ctx): State<AppContext>) -> Response {
    ctx.bluetooth.disable().await;
    get_snapshot(&ctx).await
}

async fn handle_post_bluetooth_toggle(State(ctx): State<AppContext>) -> Response {
    let has_voip = ctx.modules.has_capability("telephony").await;
    ctx.bluetooth.toggle(has_voip).await;
    get_snapshot(&ctx).await
}

async fn handle_post_bluetooth_play_pause(State(ctx): State<AppContext>) -> Response {
    ctx.bluetooth.play_pause().await;
    get_snapshot(&ctx).await
}

async fn handle_post_bluetooth_play(State(ctx): State<AppContext>) -> Response {
    ctx.bluetooth.play().await;
    get_snapshot(&ctx).await
}

async fn handle_post_bluetooth_pause(State(ctx): State<AppContext>) -> Response {
    ctx.bluetooth.pause().await;
    get_snapshot(&ctx).await
}

async fn handle_post_bluetooth_next(State(ctx): State<AppContext>) -> Response {
    ctx.bluetooth.next().await;
    get_snapshot(&ctx).await
}

async fn handle_post_bluetooth_prev(State(ctx): State<AppContext>) -> Response {
    ctx.bluetooth.prev().await;
    get_snapshot(&ctx).await
}

async fn handle_post_eject(State(ctx): State<AppContext>) -> Response {
    stop_playback_unsafe(ctx.state.clone()).await;
    {
        let mut s = ctx.state.write().await;
        s.state = "no_disc".to_string();
        s.tracks.clear();
        s.album_title.clear();
        s.album_artist.clear();
        s.cover_url.clear();
        s.last_eject_time = std::time::Instant::now();
    }

    tokio::task::spawn_blocking(|| {
        let dev = get_cd_device();
        let _ = Command::new("umount").arg(&dev).output();
        let _ = Command::new("eject").args(["-i", "0", &dev]).output();
        let _ = Command::new("eject").arg(&dev).output();
    });

    get_snapshot(&ctx).await
}

async fn handle_post_load(State(ctx): State<AppContext>) -> Response {
    stop_playback_unsafe(ctx.state.clone()).await;
    {
        let mut s = ctx.state.write().await;
        s.state = "idle".to_string();
    }

    tokio::task::spawn_blocking(|| {
        let dev = get_cd_device();
        let _ = Command::new("eject").args(["-t", &dev]).output();
    });

    get_snapshot(&ctx).await
}

async fn handle_post_refresh(State(ctx): State<AppContext>) -> Response {
    let sr1 = ctx.state.clone();
    let sr2 = ctx.state.clone();
    tokio::spawn(async move {
        let res = tokio::task::spawn_blocking(move || {
            get_track_list(Some(sr1))
        }).await;
        if let Ok((t, _, _, _, _)) = res {
            let mut s = sr2.write().await;
            s.tracks = t;
            if !s.tracks.is_empty() {
                if s.state == "no_disc" {
                    s.state = "idle".to_string();
                }
            } else {
                s.state = "no_disc".to_string();
            }
        }
    });

    get_snapshot(&ctx).await
}

async fn handle_post_set_buffer(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let ms = params
        .get("buffer_ms")
        .and_then(|v| v.as_i64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
        .unwrap_or(2500) as i32;

    let ms = ms.clamp(50, 10000);

    let (should_restart, cur_track, curr_pos) = {
        let mut s = ctx.state.write().await;
        s.buffer_ms = ms;
        let is_p = s.state == "playing";
        let pos = (s.bytes_played as f64) / 176400.0;
        (is_p, s.current_track, pos)
    };

    if should_restart {
        start_playback_unsafe(ctx.state.clone(), cur_track, curr_pos).await;
    }

    get_snapshot(&ctx).await
}

async fn handle_post_set_release(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let new_title = params
        .get("title")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string();
    let new_artist = params
        .get("artist")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string();
    let new_cover = params
        .get("cover_url")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_string();

    let mut s = ctx.state.write().await;
    if !new_title.is_empty() {
        s.album_title = new_title;
    }
    if !new_artist.is_empty() {
        s.album_artist = new_artist;
    }
    if !new_cover.is_empty() {
        s.cover_url = new_cover;
    }

    if let Some(serde_json::Value::Array(tracks_arr)) = params.get("tracks") {
        let mut map: HashMap<i32, (String, String)> = HashMap::new();
        for tr_val in tracks_arr {
            if let Some(obj) = tr_val.as_object() {
                let tn = obj.get("track").and_then(|v| v.as_i64()).map(|v| v as i32);
                let title = obj
                    .get("title")
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    .to_string();
                let artist = obj
                    .get("artist")
                    .and_then(|v| v.as_str())
                    .unwrap_or_default()
                    .to_string();
                if let Some(num) = tn {
                    map.insert(num, (title, artist));
                }
            }
        }

        for tr in &mut s.tracks {
            if let Some((title, artist)) = map.get(&tr.track) {
                if !title.is_empty() {
                    tr.title = title.clone();
                    if let Ok(c_title) = std::ffi::CString::new(title.clone()) {
                        unsafe {
                            crate::cdda_ffi::cdda_set_track_title(tr.track, c_title.as_ptr());
                        }
                    }
                }
                if !artist.is_empty() {
                    tr.artist = artist.clone();
                }
            }
        }
    }

    get_snapshot(&ctx).await
}

// --- Study Mode Routes ---

async fn handle_study_enable(State(ctx): State<AppContext>) -> Response {
    let mut s = ctx.state.write().await;
    s.study_mode_active = true;
    get_snapshot(&ctx).await
}

async fn handle_study_disable(State(ctx): State<AppContext>) -> Response {
    {
        let mut s = ctx.state.write().await;
        s.study_mode_active = false;
    }
    ctx.modules.stop_study_stream(ctx.state.clone());
    get_snapshot(&ctx).await
}

async fn handle_study_lofi_start(State(ctx): State<AppContext>) -> Response {
    let m = ctx.modules.clone();
    let s = ctx.state.clone();
    tokio::spawn(async move {
        m.start_study_stream("lofi", s).await;
    });
    get_snapshot(&ctx).await
}

async fn handle_study_lofi_stop(State(ctx): State<AppContext>) -> Response {
    ctx.modules.stop_study_stream(ctx.state.clone());
    get_snapshot(&ctx).await
}

async fn handle_study_whitenoise_start(State(ctx): State<AppContext>) -> Response {
    let m = ctx.modules.clone();
    let s = ctx.state.clone();
    tokio::spawn(async move {
        m.start_study_stream("whitenoise", s).await;
    });
    get_snapshot(&ctx).await
}

async fn handle_study_whitenoise_stop(State(ctx): State<AppContext>) -> Response {
    ctx.modules.stop_study_stream(ctx.state.clone());
    get_snapshot(&ctx).await
}

async fn handle_study_volume(State(ctx): State<AppContext>, req: Request) -> Response {
    let params = parse_params(req).await;
    let vol = params
        .get("volume")
        .and_then(|v| v.as_i64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
        .unwrap_or(80) as i32;

    let vol = vol.clamp(0, 100);
    {
        let mut s = ctx.state.write().await;
        s.aux_volume = vol;
    }

    tokio::task::spawn_blocking(move || {
        let arg = format!("{}%", vol);
        let _ = Command::new("amixer")
            .args(["-c", "Headphones", "set", "PCM", &arg])
            .output();
        let _ = Command::new("amixer")
            .args(["-c", "0", "set", "PCM", &arg])
            .output();
    });

    get_snapshot(&ctx).await
}

// --- Static File Fallback ---

async fn handle_static_fallback(State(ctx): State<AppContext>, req: Request) -> Response {
    let path = req.uri().path().trim_start_matches('/');
    let filename = if path.is_empty() {
        ctx.default_file
    } else {
        path
    };

    if filename.contains("..") || filename.starts_with('/') {
        return (StatusCode::FORBIDDEN, "Forbidden").into_response();
    }

    let file_path = PathBuf::from(filename);
    if file_path.is_file() {
        if let Ok(bytes) = fs::read(&file_path) {
            let mime = if filename.ends_with(".css") {
                "text/css"
            } else if filename.ends_with(".js") {
                "application/javascript"
            } else if filename.ends_with(".html") {
                "text/html"
            } else if filename.ends_with(".json") {
                "application/json"
            } else if filename.ends_with(".png") {
                "image/png"
            } else if filename.ends_with(".jpg") || filename.ends_with(".jpeg") {
                "image/jpeg"
            } else if filename.ends_with(".svg") {
                "image/svg+xml"
            } else {
                "application/octet-stream"
            };

            let mut res = (StatusCode::OK, bytes).into_response();
            let headers = res.headers_mut();
            if let Ok(val) = HeaderValue::from_str(mime) {
                headers.insert(header::CONTENT_TYPE, val);
            }
            headers.insert(header::ACCESS_CONTROL_ALLOW_ORIGIN, HeaderValue::from_static("*"));
            return res;
        }
    }

    (StatusCode::NOT_FOUND, "Not Found").into_response()
}
