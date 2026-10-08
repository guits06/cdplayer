#!/usr/bin/env python3
import sys
import subprocess
import time
import signal
import argparse

def main():
    parser = argparse.ArgumentParser(description="Hi-Fi RPi Audio Receiver (GStreamer RTP)")
    parser.add_argument("--dac", default="AUDIO", help="ALSA DAC device name (default: AUDIO)")
    parser.add_argument("--port", type=int, default=3000, help="UDP port (default: 3000)")
    parser.add_argument("--mode", choices=["l24", "opus"], default="l24", help="Audio mode: l24 (PCM 24-bit) or opus (Opus Hi-Fi)")
    args = parser.parse_args()

    alsa_dev = f"plughw:{args.dac}" if args.dac != "default" else "default"

    if args.mode == "opus":
        pipeline = [
            "gst-launch-1.0", "-q",
            "udpsrc", f"port={args.port}",
            'caps=application/x-rtp,media=audio,clock-rate=48000,encoding-name=OPUS,payload=96',
            "!", "rtpopusdepay",
            "!", "opusdec",
            "!", "queue", "max-size-buffers=2", "max-size-bytes=0", "max-size-time=20000000",
            "!", "audioconvert",
            "!", "audioresample",
            "!", "alsasink", f"device={alsa_dev}", "sync=false",
            "buffer-time=20000", "latency-time=10000"
        ]
    else:  # l24 PCM
        pipeline = [
            "gst-launch-1.0", "-q",
            "udpsrc", f"port={args.port}",
            'caps=application/x-rtp,media=audio,clock-rate=48000,encoding-name=L24,channels=2',
            "!", "rtpL24depay",
            "!", "queue", "max-size-buffers=2", "max-size-bytes=0", "max-size-time=20000000",
            "!", "audioconvert",
            "!", "audioresample",
            "!", "alsasink", f"device={alsa_dev}", "sync=false",
            "buffer-time=20000", "latency-time=10000"
        ]

    print(f"Starting GStreamer RTP Receiver (Mode: {args.mode}, Port: {args.port}, Device: {alsa_dev})...")
    proc = subprocess.Popen(pipeline)

    def shutdown(signum, frame):
        print("Stopping GStreamer RTP Receiver...")
        try:
            proc.terminate()
            proc.wait(timeout=1.0)
        except Exception:
            try: proc.kill()
            except Exception: pass
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    proc.wait()

if __name__ == "__main__":
    main()
