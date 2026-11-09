mod bluetooth;
mod cd_monitor;
mod cdda_ffi;
mod hardware;
mod modules;
mod musicbrainz;
mod pc_audio;
mod playback;
mod server;
mod state;

use bluetooth::BluetoothController;
use cd_monitor::spawn_cd_insertion_monitor;
use hardware::{detect_usb_dac, get_cd_device, sync_external_volume};
use modules::ModuleManager;
use playback::stop_playback_unsafe;
use server::{create_router, AppContext};
use state::PlayerState;
use std::env;
use std::ffi::CString;
use std::fs::{self, OpenOptions};
use std::io::{Seek, SeekFrom, Write};
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;
use tokio::signal;
use tokio::sync::RwLock;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 1. Force C locale for all subprocs (setcd, cd-info, etc)
    env::set_var("LC_ALL", "C");
    env::set_var("LANG", "C");

    // 2. Rotate main.log if > 2MB
    let log_path = PathBuf::from("main.log");
    if let Ok(meta) = fs::metadata(&log_path) {
        if meta.len() > 2 * 1024 * 1024 {
            if let Ok(mut file) = OpenOptions::new().read(true).write(true).open(&log_path) {
                let _ = file.seek(SeekFrom::End(-512 * 1024));
                let mut tail = Vec::new();
                use std::io::Read;
                let _ = file.read_to_end(&mut tail);
                if let Ok(mut wfile) = OpenOptions::new().write(true).truncate(true).open(&log_path) {
                    let _ = wfile.write_all(b"--- [LOG TRUNCATED AT STARTUP] ---\n");
                    let _ = wfile.write_all(&tail);
                }
            }
        }
    }

    // 3. Setup Tracing
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::from_default_env()
                .add_directive(tracing::Level::INFO.into()),
        )
        .init();

    tracing::info!("Iniciando backend nativo en Rust para RPi CD Player...");

    // 4. Initialize native C engine
    let dev = get_cd_device();
    if let Ok(c_dev) = CString::new(dev.clone()) {
        unsafe {
            let res = cdda_ffi::cdda_init(c_dev.as_ptr(), 2);
            if res == 0 {
                tracing::info!("Motor nativo C CDDA cargado con éxito (Bit-Perfect ALSA, buffer 8MB, velocidad 2x fija).");
            } else {
                tracing::warn!("cdda_init retornó {}", res);
            }
        }
    }

    // 5. Initialize shared state, modules, and bluetooth
    let state = Arc::new(RwLock::new(PlayerState::new()));
    let modules_dir = env::current_dir()?.join("modules");
    let modules = Arc::new(ModuleManager::new(modules_dir));
    let bluetooth = Arc::new(BluetoothController::new(state.clone()));

    // Initial DAC and ALSA volume sync
    let dac_name = {
        let mut s = state.write().await;
        let d = detect_usb_dac(s.manual_dac, &s.dac_name);
        s.dac_name = d.clone();
        d
    };
    sync_external_volume(100, 100, false, false, &dac_name);

    // 6. Spawn CD insertion monitoring task
    spawn_cd_insertion_monitor(state.clone());

    // 7. Setup Web Servers
    let ctx_8000 = AppContext {
        state: state.clone(),
        modules: modules.clone(),
        bluetooth: bluetooth.clone(),
        default_file: "index.html",
    };
    let app_8000 = create_router(ctx_8000);

    let ctx_8001 = AppContext {
        state: state.clone(),
        modules: modules.clone(),
        bluetooth: bluetooth.clone(),
        default_file: "visualizer.html",
    };
    let app_8001 = create_router(ctx_8001);

    let addr_8000: SocketAddr = "0.0.0.0:8000".parse()?;
    let addr_8001: SocketAddr = "0.0.0.0:8001".parse()?;

    tracing::info!("CD Player Web Control Server running on port 8000 and 8001 (Visualizer)...");

    let listener_8000 = tokio::net::TcpListener::bind(addr_8000).await?;
    let listener_8001 = tokio::net::TcpListener::bind(addr_8001).await?;

    let server_8000 = tokio::spawn(async move {
        let _ = axum::serve(listener_8000, app_8000).await;
    });

    let server_8001 = tokio::spawn(async move {
        let _ = axum::serve(listener_8001, app_8001).await;
    });

    // 8. Graceful Shutdown Handler
    let shutdown_signal = async {
        let ctrl_c = async {
            signal::ctrl_c().await.expect("failed to install Ctrl+C handler");
        };

        #[cfg(unix)]
        let terminate = async {
            signal::unix::signal(signal::unix::SignalKind::terminate())
                .expect("failed to install signal handler")
                .recv()
                .await;
        };

        #[cfg(not(unix))]
        let terminate = std::future::pending::<()>();

        tokio::select! {
            _ = ctrl_c => {},
            _ = terminate => {},
        }
    };

    tokio::select! {
        _ = shutdown_signal => {
            tracing::info!("Señal de apagado recibida. Deteniendo reproductor...");
        }
        _ = server_8000 => {}
        _ = server_8001 => {}
    }

    // Cleanup on exit
    stop_playback_unsafe(state.clone()).await;
    unsafe {
        cdda_ffi::cdda_close();
    }
    tracing::info!("Servidores detenidos correctamente.");

    Ok(())
}
