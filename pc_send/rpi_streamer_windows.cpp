#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>

#include <mmsystem.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

static std::atomic<bool> g_running(true);

static BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// Latency Ping Thread
static void ping_thread_func(const std::string& target_ip, int port, std::atomic<double>* out_latency) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    DWORD timeout = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

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
        int from_len = sizeof(from);
        char recv_buf[64];
        int n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (sockaddr*)&from, &from_len);
        if (n >= (int)(4 + sizeof(t0)) && std::memcmp(recv_buf, "PONG", 4) == 0) {
            uint64_t orig_t0 = 0;
            std::memcpy(&orig_t0, recv_buf + 4, sizeof(orig_t0));
            uint64_t t1 = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            double rtt_ms = (t1 - orig_t0) / 1000.0;
            out_latency->store(rtt_ms);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    closesocket(sock);
}

int main(int argc, char* argv[]) {
    std::string target_ip = "raspberrypi.local";
    int target_port = 3000;

    if (argc > 1) target_ip = argv[1];
    if (argc > 2) target_port = std::stoi(argv[2]);

    SetConsoleCtrlHandler(console_handler, TRUE);

    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    std::cout << "=== RPi C++ Windows WASAPI Loopback Streamer (Audio Master -> RTP L24) ===" << std::endl;
    std::cout << "Destino: " << target_ip << ":" << target_port << std::endl;

    // Initialize WinSock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Error iniciando WinSock2" << std::endl;
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Error creando socket UDP" << std::endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(target_port);
    inet_pton(AF_INET, target_ip.c_str(), &dest_addr.sin_addr);

    // Initialize COM & WASAPI Loopback Capture
    CoInitialize(nullptr);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator
    );
    if (FAILED(hr)) {
        std::cerr << "Error inicializando MMDeviceEnumerator" << std::endl;
        return 1;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        std::cerr << "Error obteniendo endpoint de audio predeterminado" << std::endl;
        enumerator->Release();
        return 1;
    }

    IAudioClient* audio_client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client);
    if (FAILED(hr)) {
        std::cerr << "Error activando IAudioClient" << std::endl;
        device->Release();
        enumerator->Release();
        return 1;
    }

    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format) {
        std::cerr << "Error obteniendo MixFormat" << std::endl;
        return 1;
    }

    std::cout << "Dispositivo Windows: " << mix_format->nSamplesPerSec << " Hz, "
              << mix_format->nChannels << " canales, "
              << mix_format->wBitsPerSample << " bits." << std::endl;

    REFERENCE_TIME hns_buffer_duration = 500000; // 50ms buffer
    hr = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hns_buffer_duration,
        0,
        mix_format,
        nullptr
    );
    if (FAILED(hr)) {
        std::cerr << "Error inicializando loopback stream (hr=0x" << std::hex << hr << ")" << std::endl;
        return 1;
    }

    IAudioCaptureClient* capture_client = nullptr;
    hr = audio_client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client);
    if (FAILED(hr)) {
        std::cerr << "Error obteniendo IAudioCaptureClient" << std::endl;
        return 1;
    }

    audio_client->Start();
    std::cout << "WASAPI Loopback iniciado con exito." << std::endl;

    // Latency Ping Thread
    std::atomic<double> latency_ms(-1.0);
    std::thread ping_th(ping_thread_func, target_ip, target_port + 1, &latency_ms);

    // 480 samples @ 48kHz = 10ms RTP packet (matches pc_audio_sender.py low mode)
    const int FRAMES_PER_PACKET = 480; 
    std::vector<uint8_t> rtp_packet(12 + FRAMES_PER_PACKET * 6);
    uint8_t* pkt = rtp_packet.data();
    pkt[0] = 0x80;
    pkt[1] = 96; // Payload Type 96 L24
    uint32_t ssrc = 0x57494E31; // "WIN1"
    std::memcpy(pkt + 8, &ssrc, 4);

    uint16_t seq_num = 1;
    uint32_t rtp_timestamp = 0;

    WORD bits = mix_format->wBitsPerSample;
    WORD channels = mix_format->nChannels;
    DWORD in_rate = mix_format->nSamplesPerSec;
    WORD block_align = mix_format->nBlockAlign;

    bool is_float = false;
    if (mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_format);
        static const GUID guid_float = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
        if (std::memcmp(&ext->SubFormat, &guid_float, sizeof(GUID)) == 0) {
            is_float = true;
        }
    }

    std::cout << "Decodificador: " << (is_float ? "IEEE Float" : "PCM Integer")
              << " " << bits << " bits, " << in_rate << " Hz -> 48000 Hz L24 stereo." << std::endl;

    std::vector<float> in_samples;
    double resample_pos = 0.0;
    std::vector<float> out_samples;
    size_t out_read_idx = 0;

    auto last_stat = std::chrono::steady_clock::now();
    uint64_t total_frames = 0;

    while (g_running) {
        bool had_data = false;

        while (true) {
            UINT32 packet_length = 0;
            hr = capture_client->GetNextPacketSize(&packet_length);
            if (FAILED(hr) || packet_length == 0) break;

            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD flags = 0;

            hr = capture_client->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            had_data = true;

            for (UINT32 f = 0; f < num_frames; f++) {
                float l = 0.0f, r = 0.0f;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data != nullptr) {
                    const BYTE* frame_ptr = data + f * block_align;
                    if (is_float && bits == 32) {
                        const float* p = reinterpret_cast<const float*>(frame_ptr);
                        l = p[0];
                        r = (channels > 1) ? p[1] : l;
                    } else if (bits == 16) {
                        const int16_t* p = reinterpret_cast<const int16_t*>(frame_ptr);
                        l = p[0] / 32768.0f;
                        r = (channels > 1) ? p[1] / 32768.0f : l;
                    } else if (bits == 24) {
                        int32_t s0 = (int32_t)((frame_ptr[0] << 8) | (frame_ptr[1] << 16) | (frame_ptr[2] << 24)) >> 8;
                        int32_t s1 = (channels > 1) ? ((int32_t)((frame_ptr[3] << 8) | (frame_ptr[4] << 16) | (frame_ptr[5] << 24)) >> 8) : s0;
                        l = s0 / 8388608.0f;
                        r = s1 / 8388608.0f;
                    } else if (bits == 32) {
                        const int32_t* p = reinterpret_cast<const int32_t*>(frame_ptr);
                        l = p[0] / 2147483648.0f;
                        r = (channels > 1) ? p[1] / 2147483648.0f : l;
                    }
                }
                in_samples.push_back(l);
                in_samples.push_back(r);
            }

            capture_client->ReleaseBuffer(num_frames);
        }

        // Exact resampling to 48000 Hz
        if (in_rate == 48000) {
            out_samples.insert(out_samples.end(), in_samples.begin(), in_samples.end());
            in_samples.clear();
        } else {
            size_t in_frames = in_samples.size() / 2;
            double step = (double)in_rate / 48000.0;
            while (resample_pos + 1.0 < (double)in_frames) {
                size_t idx = (size_t)resample_pos;
                float frac = (float)(resample_pos - idx);
                float l = in_samples[idx * 2 + 0] * (1.0f - frac) + in_samples[(idx + 1) * 2 + 0] * frac;
                float r = in_samples[idx * 2 + 1] * (1.0f - frac) + in_samples[(idx + 1) * 2 + 1] * frac;
                out_samples.push_back(l);
                out_samples.push_back(r);
                resample_pos += step;
            }
            size_t consumed_frames = (size_t)resample_pos;
            if (consumed_frames > 0) {
                in_samples.erase(in_samples.begin(), in_samples.begin() + consumed_frames * 2);
                resample_pos -= consumed_frames;
            }
        }

        // Emit RTP packets
        while ((out_samples.size() - out_read_idx) >= (size_t)(FRAMES_PER_PACKET * 2)) {
            pkt[2] = (seq_num >> 8) & 0xFF;
            pkt[3] = seq_num & 0xFF;
            pkt[4] = (rtp_timestamp >> 24) & 0xFF;
            pkt[5] = (rtp_timestamp >> 16) & 0xFF;
            pkt[6] = (rtp_timestamp >> 8) & 0xFF;
            pkt[7] = rtp_timestamp & 0xFF;

            uint8_t* payload = pkt + 12;
            for (int i = 0; i < FRAMES_PER_PACKET; i++) {
                float l = out_samples[out_read_idx + i * 2 + 0];
                float r = out_samples[out_read_idx + i * 2 + 1];

                if (l > 1.0f) l = 1.0f;
                else if (l < -1.0f) l = -1.0f;

                if (r > 1.0f) r = 1.0f;
                else if (r < -1.0f) r = -1.0f;

                int32_t il = (int32_t)(l * 8388607.0f);
                int32_t ir = (int32_t)(r * 8388607.0f);

                payload[i * 6 + 0] = (uint8_t)((il >> 16) & 0xFF);
                payload[i * 6 + 1] = (uint8_t)((il >> 8) & 0xFF);
                payload[i * 6 + 2] = (uint8_t)(il & 0xFF);

                payload[i * 6 + 3] = (uint8_t)((ir >> 16) & 0xFF);
                payload[i * 6 + 4] = (uint8_t)((ir >> 8) & 0xFF);
                payload[i * 6 + 5] = (uint8_t)(ir & 0xFF);
            }

            sendto(sock, (const char*)pkt, (int)rtp_packet.size(), 0, (sockaddr*)&dest_addr, sizeof(dest_addr));

            seq_num++;
            rtp_timestamp += FRAMES_PER_PACKET;
            total_frames += FRAMES_PER_PACKET;
            out_read_idx += FRAMES_PER_PACKET * 2;

            if (out_read_idx > 8192) {
                out_samples.erase(out_samples.begin(), out_samples.begin() + out_read_idx);
                out_read_idx = 0;
            }
        }

        if (!had_data) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stat).count() >= 2) {
            double lat = latency_ms.load();
            std::cout << "\r[Streaming RTP L24] Frames: " << total_frames
                      << " | Red RTT: " << (lat >= 0 ? (std::to_string((int)lat) + " ms") : "calculando...")
                      << "       " << std::flush;
            last_stat = now;
        }
    }

    std::cout << "\nCerrando captura WASAPI..." << std::endl;
    audio_client->Stop();
    if (ping_th.joinable()) ping_th.join();

    if (capture_client) capture_client->Release();
    if (audio_client) audio_client->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    CoTaskMemFree(mix_format);
    CoUninitialize();

    closesocket(sock);
    WSACleanup();
    timeEndPeriod(1);

    return 0;
}
