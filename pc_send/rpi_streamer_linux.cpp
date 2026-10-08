#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <cmath>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <pulse/simple.h>
#include <pulse/error.h>

static std::atomic<bool> g_running(true);

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

// Latency Ping Thread
static void ping_thread_func(const std::string& target_ip, int port, std::atomic<double>* out_latency) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in target_addr;
    std::memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(port);
    inet_pton(AF_INET, target_ip.c_str(), &target_addr.sin_addr);

    while (g_running) {
        uint64_t t0 = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        char buf[64];
        std::memcpy(buf, "PING", 4);
        std::memcpy(buf + 4, &t0, sizeof(t0));
        sendto(sock, buf, 4 + sizeof(t0), 0, (sockaddr*)&target_addr, sizeof(target_addr));

        sockaddr_in from;
        socklen_t from_len = sizeof(from);
        char recv_buf[64];
        ssize_t n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (sockaddr*)&from, &from_len);
        if (n >= (ssize_t)(4 + sizeof(t0)) && std::memcmp(recv_buf, "PONG", 4) == 0) {
            uint64_t orig_t0 = 0;
            std::memcpy(&orig_t0, recv_buf + 4, sizeof(orig_t0));
            uint64_t t1 = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            double rtt_ms = (t1 - orig_t0) / 1000.0;
            out_latency->store(rtt_ms);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    close(sock);
}

int main(int argc, char* argv[]) {
    std::string target_ip = "raspberrypi.local";
    int target_port = 3000;
    std::string device_name = ""; // empty will use default sink monitor

    if (argc > 1) target_ip = argv[1];
    if (argc > 2) target_port = std::stoi(argv[2]);
    if (argc > 3) device_name = argv[3];

    std::string capture_device = device_name.empty() ? "@DEFAULT_SINK@.monitor" : device_name;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    std::cout << "=== RPi C++ Linux Audio Streamer (PulseAudio/PipeWire -> RTP L24) ===" << std::endl;
    std::cout << "Destino: " << target_ip << ":" << target_port << std::endl;
    std::cout << "Fuente de captura: " << capture_device << " (Audio del sistema / salida de altavoces)" << std::endl;

    // UDP Socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Error creando socket UDP" << std::endl;
        return 1;
    }

    // Set high socket buffer to avoid packet loss on local transmission
    int sndbuf = 1048576; // 1 MB
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(target_port);
    inet_pton(AF_INET, target_ip.c_str(), &dest_addr.sin_addr);

    // PulseAudio / PipeWire Capture Setup (48000 Hz, 2 channels, Float32 native)
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = 48000;
    ss.channels = 2;

    // 5ms buffer fragmentation for ultra-low latency capture
    const int FRAMES_PER_PACKET = 240; // 5ms @ 48kHz
    const int BYTES_PER_PACKET = FRAMES_PER_PACKET * sizeof(float) * 2; // 1920 bytes
    pa_buffer_attr ba;
    std::memset(&ba, 0xFF, sizeof(ba));
    ba.maxlength = BYTES_PER_PACKET * 2;  // 2 packets max in ring buffer
    ba.fragsize  = BYTES_PER_PACKET;      // deliver every 5ms
    // tlength/prebuf/minreq are playback-only; keep as -1 for record streams

    int pa_error = 0;
    pa_simple* pa = pa_simple_new(
        nullptr,
        "RPi-CD-Player-Streamer",
        PA_STREAM_RECORD,
        capture_device.c_str(),
        "Desktop Audio Monitor",
        &ss,
        nullptr,
        &ba,
        &pa_error
    );

    if (!pa) {
        std::cerr << "Error conectando a PulseAudio/PipeWire en [" << capture_device << "]: " << pa_strerror(pa_error) << std::endl;
        close(sock);
        return 1;
    }

    std::cout << "Conectado a PulseAudio/PipeWire (Float32 Bit-Perfect -> L24, 48000Hz stereo)." << std::endl;
    std::cout << "Buffer de captura optimizado para latencia minima (~5 ms)." << std::endl;

    // Start Latency Monitor Thread
    std::atomic<double> latency_ms(-1.0);
    std::thread ping_th(ping_thread_func, target_ip, target_port + 1, &latency_ms);

    // Frame parameters: 240 frames = 5 ms at 48000 Hz
    std::vector<float> raw_buf(FRAMES_PER_PACKET * 2);
    std::vector<uint8_t> rtp_packet(12 + FRAMES_PER_PACKET * 6); // 12 header + 6 bytes/frame (L24 stereo)

    // RTP Header initialization
    uint16_t seq_num = 1000;
    uint32_t rtp_timestamp = 0;
    uint32_t ssrc = 0x52504931; // "RPI1"

    uint8_t* pkt = rtp_packet.data();
    pkt[0] = 0x80; // V=2, P=0, X=0, CC=0
    pkt[1] = 96;   // Payload type 96 (L24 dynamic)
    std::memcpy(pkt + 8, &ssrc, 4);

    uint64_t total_frames = 0;
    auto last_stat = std::chrono::steady_clock::now();

    while (g_running) {
        if (pa_simple_read(pa, raw_buf.data(), raw_buf.size() * sizeof(float), &pa_error) < 0) {
            std::cerr << "pa_simple_read error: " << pa_strerror(pa_error) << std::endl;
            break;
        }

        // Pack sequence number & timestamp (Big Endian)
        pkt[2] = (seq_num >> 8) & 0xFF;
        pkt[3] = seq_num & 0xFF;
        pkt[4] = (rtp_timestamp >> 24) & 0xFF;
        pkt[5] = (rtp_timestamp >> 16) & 0xFF;
        pkt[6] = (rtp_timestamp >> 8) & 0xFF;
        pkt[7] = rtp_timestamp & 0xFF;

        // Convert PCM samples to RFC 3190 L24 format: 24-bit Big-Endian (Left, then Right)
        uint8_t* payload = pkt + 12;
        for (int f = 0; f < FRAMES_PER_PACKET; f++) {
            float left = raw_buf[f * 2 + 0];
            float right = raw_buf[f * 2 + 1];

            // Safety clamp to prevent digital clipping / wrap-around
            if (left > 1.0f) left = 1.0f;
            else if (left < -1.0f) left = -1.0f;

            if (right > 1.0f) right = 1.0f;
            else if (right < -1.0f) right = -1.0f;

            int32_t val_l = (int32_t)(left * 8388607.0f);
            int32_t val_r = (int32_t)(right * 8388607.0f);

            // Left 24-bit MSB..LSB
            payload[f * 6 + 0] = (uint8_t)((val_l >> 16) & 0xFF);
            payload[f * 6 + 1] = (uint8_t)((val_l >> 8) & 0xFF);
            payload[f * 6 + 2] = (uint8_t)(val_l & 0xFF);

            // Right 24-bit MSB..LSB
            payload[f * 6 + 3] = (uint8_t)((val_r >> 16) & 0xFF);
            payload[f * 6 + 4] = (uint8_t)((val_r >> 8) & 0xFF);
            payload[f * 6 + 5] = (uint8_t)(val_r & 0xFF);
        }

        sendto(sock, pkt, rtp_packet.size(), 0, (sockaddr*)&dest_addr, sizeof(dest_addr));

        seq_num++;
        rtp_timestamp += FRAMES_PER_PACKET;
        total_frames += FRAMES_PER_PACKET;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stat).count() >= 2) {
            double lat = latency_ms.load();
            std::cout << "\r[Streaming RTP L24] Frames: " << total_frames
                      << " | Red RTT: " << (lat >= 0 ? (std::to_string((int)lat) + " ms") : "calculando...")
                      << "       " << std::flush;
            last_stat = now;
        }
    }

    std::cout << "\nDeteniendo emisor de audio..." << std::endl;
    if (ping_th.joinable()) ping_th.join();
    if (pa) pa_simple_free(pa);
    close(sock);

    return 0;
}
