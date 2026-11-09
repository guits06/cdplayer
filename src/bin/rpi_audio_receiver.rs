use clap::Parser;
use std::process::{Command, Stdio};
use tokio::signal;

#[derive(Parser, Debug)]
#[command(
    name = "rpi_audio_receiver",
    about = "Hi-Fi RPi Audio Receiver (GStreamer RTP)"
)]
struct Args {
    #[arg(long, default_value = "AUDIO", help = "ALSA DAC device name (default: AUDIO)")]
    dac: String,

    #[arg(long, default_value_t = 3000, help = "UDP port (default: 3000)")]
    port: u16,

    #[arg(
        long,
        default_value = "l24",
        help = "Audio mode: l24 (PCM 24-bit) or opus (Opus Hi-Fi)"
    )]
    mode: String,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    let alsa_dev = if args.dac != "default" {
        format!("plughw:{}", args.dac)
    } else {
        "default".to_string()
    };

    let pipeline_args: Vec<String> = if args.mode == "opus" {
        vec![
            "-q".into(),
            "udpsrc".into(),
            format!("port={}", args.port),
            "caps=application/x-rtp,media=audio,clock-rate=48000,encoding-name=OPUS,payload=96".into(),
            "!".into(),
            "rtpopusdepay".into(),
            "!".into(),
            "opusdec".into(),
            "!".into(),
            "queue".into(),
            "max-size-buffers=2".into(),
            "max-size-bytes=0".into(),
            "max-size-time=20000000".into(),
            "!".into(),
            "audioconvert".into(),
            "!".into(),
            "audioresample".into(),
            "!".into(),
            "alsasink".into(),
            format!("device={}", alsa_dev),
            "sync=false".into(),
            "buffer-time=20000".into(),
            "latency-time=10000".into(),
        ]
    } else {
        vec![
            "-q".into(),
            "udpsrc".into(),
            format!("port={}", args.port),
            "caps=application/x-rtp,media=audio,clock-rate=48000,encoding-name=L24,channels=2".into(),
            "!".into(),
            "rtpL24depay".into(),
            "!".into(),
            "queue".into(),
            "max-size-buffers=2".into(),
            "max-size-bytes=0".into(),
            "max-size-time=20000000".into(),
            "!".into(),
            "audioconvert".into(),
            "!".into(),
            "audioresample".into(),
            "!".into(),
            "alsasink".into(),
            format!("device={}", alsa_dev),
            "sync=false".into(),
            "buffer-time=20000".into(),
            "latency-time=10000".into(),
        ]
    };

    println!(
        "Starting GStreamer RTP Receiver (Mode: {}, Port: {}, Device: {})...",
        args.mode, args.port, alsa_dev
    );

    let mut child = Command::new("gst-launch-1.0")
        .args(&pipeline_args)
        .stdin(Stdio::null())
        .spawn()?;

    let ctrl_c = signal::ctrl_c();
    #[cfg(unix)]
    let mut sigterm = signal::unix::signal(signal::unix::SignalKind::terminate())?;

    tokio::select! {
        _ = ctrl_c => {
            println!("Stopping GStreamer RTP Receiver...");
        }
        _ = sigterm.recv() => {
            println!("Stopping GStreamer RTP Receiver...");
        }
    }

    let _ = child.kill();
    let _ = child.wait();

    Ok(())
}
