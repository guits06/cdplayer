#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/cdrom.h>
#include <alsa/asoundlib.h>
#include <glob.h>

#include "cdda_engine.h"

// Ring Buffer: 3500 sectors * 2352 bytes = 8,232,000 bytes (~46.6 seconds of audio)
#define RING_BUF_BYTES (RING_BUFFER_SECTORS * CD_FRAME_SIZE)
#define READ_BATCH_SECTORS 16 // Read 16 sectors (~37.6 KB) per batch
#define READ_BATCH_BYTES (READ_BATCH_SECTORS * CD_FRAME_SIZE)

typedef struct {
    uint8_t data[RING_BUF_BYTES];
    size_t write_pos;
    size_t read_pos;
    size_t count;
    bool eof;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} RingBuffer;

typedef struct {
    char device_path[256];
    int speed_x;
    int cd_fd;
    CDDiscInfo disc_info;
    bool initialized;
    
    // Playback state
    int state; // 0: idle, 1: playing, 2: paused, 3: starting, -1: error, -3: dac error
    int session_id;
    int current_track;
    int playback_initial_lba;
    uint64_t frames_played; // Audio frames written to ALSA (1 frame = 4 bytes)
    double duration_sec;
    char error_msg[256];
    char alsa_device[64];
    snd_pcm_t* active_pcm;

    // Reader state for gapless Red Book playback
    int reader_track;
    int current_read_lba;
    int reader_end_lba;

    // Threading
    pthread_t reader_thread;
    pthread_t player_thread;
    bool threads_running;
    bool stop_requested;
    bool pause_requested;
    pthread_mutex_t state_mutex;

    RingBuffer ring;
} EngineState;

static EngineState g_engine = {0};

// --- RING BUFFER IMPLEMENTATION ---

static void ring_init(RingBuffer* rb) {
    rb->write_pos = 0;
    rb->read_pos = 0;
    rb->count = 0;
    rb->eof = false;
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);
}

static void ring_reset(RingBuffer* rb) {
    pthread_mutex_lock(&rb->mutex);
    rb->write_pos = 0;
    rb->read_pos = 0;
    rb->count = 0;
    rb->eof = false;
    pthread_cond_broadcast(&rb->not_full);
    pthread_mutex_unlock(&rb->mutex);
}

static void ring_set_eof(RingBuffer* rb) {
    pthread_mutex_lock(&rb->mutex);
    rb->eof = true;
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->mutex);
}

static size_t ring_write(RingBuffer* rb, const uint8_t* src, size_t bytes, bool* stop_flag) {
    size_t written = 0;
    while (written < bytes) {
        pthread_mutex_lock(&rb->mutex);
        while (rb->count >= RING_BUF_BYTES && !(*stop_flag)) {
            pthread_cond_wait(&rb->not_full, &rb->mutex);
        }
        if (*stop_flag) {
            pthread_mutex_unlock(&rb->mutex);
            break;
        }

        size_t available = RING_BUF_BYTES - rb->count;
        size_t to_write = bytes - written;
        if (to_write > available) to_write = available;

        size_t part1 = RING_BUF_BYTES - rb->write_pos;
        if (part1 > to_write) part1 = to_write;
        size_t part2 = to_write - part1;

        memcpy(rb->data + rb->write_pos, src + written, part1);
        if (part2 > 0) {
            memcpy(rb->data, src + written + part1, part2);
        }

        rb->write_pos = (rb->write_pos + to_write) % RING_BUF_BYTES;
        rb->count += to_write;
        written += to_write;

        pthread_cond_signal(&rb->not_empty);
        pthread_mutex_unlock(&rb->mutex);
    }
    return written;
}

static size_t ring_read(RingBuffer* rb, uint8_t* dst, size_t bytes, bool* stop_flag) {
    size_t bytes_read = 0;
    while (bytes_read < bytes) {
        pthread_mutex_lock(&rb->mutex);
        while (rb->count == 0 && !(*stop_flag) && !rb->eof) {
            pthread_cond_wait(&rb->not_empty, &rb->mutex);
        }
        if (rb->count == 0 && (*stop_flag || rb->eof)) {
            pthread_mutex_unlock(&rb->mutex);
            break;
        }

        size_t available = rb->count;
        size_t to_read = bytes - bytes_read;
        if (to_read > available) to_read = available;

        size_t part1 = RING_BUF_BYTES - rb->read_pos;
        if (part1 > to_read) part1 = to_read;
        size_t part2 = to_read - part1;

        memcpy(dst + bytes_read, rb->data + rb->read_pos, part1);
        if (part2 > 0) {
            memcpy(dst + bytes_read + part1, rb->data, part2);
        }

        rb->read_pos = (rb->read_pos + to_read) % RING_BUF_BYTES;
        rb->count -= to_read;
        bytes_read += to_read;

        pthread_cond_signal(&rb->not_full);
        pthread_mutex_unlock(&rb->mutex);
    }
    return bytes_read;
}

static int ring_fill_percentage(RingBuffer* rb) {
    pthread_mutex_lock(&rb->mutex);
    int pct = (int)((rb->count * 100ULL) / RING_BUF_BYTES);
    pthread_mutex_unlock(&rb->mutex);
    return pct;
}

// --- HARDWARE SPEED & LOW-LEVEL ACCESS ---

static int set_hardware_speed(int fd, int speed) {
    if (fd < 0) return -1;
    // Standard Linux CD-ROM ioctl (speed in units of 150 KB/s: 2 = 300 KB/s / ~450 RPM)
    return ioctl(fd, CDROM_SELECT_SPEED, speed);
}

// Read raw CDDA sectors via Linux ioctl CDROMREADAUDIO or SCSI MMC-3 fallback
static int read_cdda_sectors(int fd, int start_lba, int nframes, uint8_t* out_buffer) {
    struct cdrom_read_audio ra;
    ra.addr.lba = start_lba;
    ra.addr_format = CDROM_LBA;
    ra.nframes = nframes;
    ra.buf = out_buffer;
    
    int ret = ioctl(fd, CDROMREADAUDIO, &ra);
    if (ret == 0) return nframes;

    // Fallback: SCSI MMC-3 READ CD (0xBE) command via CDROM_SEND_PACKET
    struct cdrom_generic_command cgc;
    memset(&cgc, 0, sizeof(cgc));
    cgc.cmd[0] = 0xBE; // READ CD
    cgc.cmd[1] = 0x04; // Expected sector type = CD-DA
    cgc.cmd[2] = (start_lba >> 24) & 0xFF;
    cgc.cmd[3] = (start_lba >> 16) & 0xFF;
    cgc.cmd[4] = (start_lba >> 8) & 0xFF;
    cgc.cmd[5] = start_lba & 0xFF;
    cgc.cmd[6] = (nframes >> 16) & 0xFF;
    cgc.cmd[7] = (nframes >> 8) & 0xFF;
    cgc.cmd[8] = nframes & 0xFF;
    cgc.cmd[9] = 0x10; // User data only (2352 bytes/sector)
    cgc.buffer = out_buffer;
    cgc.buflen = nframes * CD_FRAME_SIZE;
    cgc.data_direction = CGC_DATA_READ;
    cgc.quiet = 1;
    cgc.timeout = 3000;

    ret = ioctl(fd, CDROM_SEND_PACKET, &cgc);
    return (ret == 0) ? nframes : -1;
}

// Read CD Table of Contents (TOC) directly from open file descriptor
static int read_disc_info_fd(int fd, CDDiscInfo* out_info) {
    if (fd < 0 || !out_info) return -1;
    memset(out_info, 0, sizeof(CDDiscInfo));

    struct cdrom_tochdr th;
    if (ioctl(fd, CDROMREADTOCHDR, &th) < 0) {
        return -2;
    }

    out_info->first_track = th.cdth_trk0;
    out_info->last_track = th.cdth_trk1;
    int count = th.cdth_trk1 - th.cdth_trk0 + 1;
    if (count > MAX_TRACKS) count = MAX_TRACKS;
    out_info->track_count = count;

    for (int i = 0; i < count; i++) {
        int trk_num = th.cdth_trk0 + i;
        struct cdrom_tocentry te;
        memset(&te, 0, sizeof(te));
        te.cdte_track = trk_num;
        te.cdte_format = CDROM_LBA;

        if (ioctl(fd, CDROMREADTOCENTRY, &te) == 0) {
            out_info->tracks[i].track_num = trk_num;
            out_info->tracks[i].start_lba = te.cdte_addr.lba;
            snprintf(out_info->tracks[i].title, sizeof(out_info->tracks[i].title), "Pista %d", trk_num);
        }
    }

    struct cdrom_tocentry te_leadout;
    memset(&te_leadout, 0, sizeof(te_leadout));
    te_leadout.cdte_track = CDROM_LEADOUT;
    te_leadout.cdte_format = CDROM_LBA;

    if (ioctl(fd, CDROMREADTOCENTRY, &te_leadout) == 0) {
        out_info->leadout_lba = te_leadout.cdte_addr.lba;
    } else {
        out_info->leadout_lba = out_info->tracks[count - 1].start_lba + 15000;
    }

    for (int i = 0; i < count; i++) {
        int next_lba = (i + 1 < count) ? out_info->tracks[i + 1].start_lba : out_info->leadout_lba;
        int sectors = next_lba - out_info->tracks[i].start_lba;
        if (sectors < 75) sectors = 75;
        out_info->tracks[i].total_sectors = sectors;
        out_info->tracks[i].duration_sec = sectors / CD_FRAMES_PER_SEC;
    }

    return count;
}

// Helper to open the CD device with automatic dynamic discovery across nodes
static int open_cd_device_with_fallback(const char* preferred_path) {
    char candidates[16][256];
    int n_candidates = 0;

    #define ADD_CANDIDATE(p) do { \
        const char* _p = (p); \
        if (_p && _p[0] && access(_p, F_OK) == 0) { \
            bool dup = false; \
            for (int k = 0; k < n_candidates; k++) { \
                if (strcmp(candidates[k], _p) == 0) { dup = true; break; } \
            } \
            if (!dup && n_candidates < 16) { \
                snprintf(candidates[n_candidates++], sizeof(candidates[0]), "%s", _p); \
            } \
        } \
    } while(0)

    if (preferred_path && preferred_path[0]) ADD_CANDIDATE(preferred_path);
    if (g_engine.device_path[0]) ADD_CANDIDATE(g_engine.device_path);

    glob_t g;
    if (glob("/dev/disk/by-id/usb-*DVD*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) ADD_CANDIDATE(g.gl_pathv[i]);
        globfree(&g);
    }
    if (glob("/dev/disk/by-id/usb-*CD*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) ADD_CANDIDATE(g.gl_pathv[i]);
        globfree(&g);
    }

    ADD_CANDIDATE("/dev/sr0");
    ADD_CANDIDATE("/dev/sr1");
    ADD_CANDIDATE("/dev/sr2");
    ADD_CANDIDATE("/dev/sr3");
    ADD_CANDIDATE("/dev/cdrom");

    for (int retry = 0; retry < 15; retry++) {
        for (int i = 0; i < n_candidates; i++) {
            int fd = open(candidates[i], O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                struct cdrom_tochdr th;
                if (ioctl(fd, CDROMREADTOCHDR, &th) == 0) {
                    snprintf(g_engine.device_path, sizeof(g_engine.device_path), "%.255s", candidates[i]);
                    return fd;
                }
                close(fd);
            }
        }
        usleep(100000); // 100ms
    }

    return -1;
}

// --- METADATA ENRICHMENT API ---

int cdda_set_track_title(int track, const char* title) {
    pthread_mutex_lock(&g_engine.state_mutex);
    if (track >= g_engine.disc_info.first_track && track <= g_engine.disc_info.last_track) {
        int idx = track - g_engine.disc_info.first_track;
        snprintf(g_engine.disc_info.tracks[idx].title, sizeof(g_engine.disc_info.tracks[idx].title), "%s", title);
    }
    pthread_mutex_unlock(&g_engine.state_mutex);
    return 0;
}

// --- WORKER THREADS ---

static void* reader_thread_func(void* arg) {
    (void)arg;
    uint8_t batch_buf[READ_BATCH_BYTES];
    int fd = g_engine.cd_fd;

    while (!g_engine.stop_requested) {
        // Gapless track boundary check
        if (g_engine.current_read_lba >= g_engine.reader_end_lba) {
            int next_track_num = g_engine.reader_track + 1;
            int next_idx = next_track_num - g_engine.disc_info.first_track;

            if (next_idx < g_engine.disc_info.track_count && next_track_num <= g_engine.disc_info.last_track) {
                // Continuous Red Book audio stream: seamlessly transition to next track
                CDTrack* next_trk = &g_engine.disc_info.tracks[next_idx];
                g_engine.reader_track = next_track_num;
                g_engine.current_read_lba = next_trk->start_lba;
                g_engine.reader_end_lba = next_trk->start_lba + next_trk->total_sectors;
            } else {
                // Last track completed: signal EOF to ring buffer
                ring_set_eof(&g_engine.ring);
                break;
            }
        }

        // Throttle reading if ring buffer is more than 85% full to conserve drive power and prevent noise
        if (ring_fill_percentage(&g_engine.ring) > 85) {
            usleep(25000); // 25ms
            continue;
        }

        int remaining = g_engine.reader_end_lba - g_engine.current_read_lba;
        int to_read = (remaining > READ_BATCH_SECTORS) ? READ_BATCH_SECTORS : remaining;
        if (to_read <= 0) to_read = READ_BATCH_SECTORS;

        int sectors_read = read_cdda_sectors(fd, g_engine.current_read_lba, to_read, batch_buf);
        if (sectors_read <= 0) {
            // Read error or retry
            usleep(30000);
            continue;
        }

        size_t bytes = (size_t)sectors_read * CD_FRAME_SIZE;
        ring_write(&g_engine.ring, batch_buf, bytes, &g_engine.stop_requested);

        g_engine.current_read_lba += sectors_read;
    }

    return NULL;
}

static void* player_thread_func(void* arg) {
    (void)arg;
    snd_pcm_t* pcm = NULL;
    int err = 0;

    const char* dev_name = (g_engine.alsa_device[0] != '\0') ? g_engine.alsa_device : "default";

    if ((err = snd_pcm_open(&pcm, dev_name, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        pthread_mutex_lock(&g_engine.state_mutex);
        if (err == -ENOENT || err == -ENODEV || err == -EBUSY) {
            snprintf(g_engine.error_msg, sizeof(g_engine.error_msg),
                     "DAC SMSL SU-1 no disponible (comprueba entrada USB en el DAC): %s", snd_strerror(err));
            g_engine.state = -3;
        } else {
            snprintf(g_engine.error_msg, sizeof(g_engine.error_msg), "ALSA open error (%s): %s", dev_name, snd_strerror(err));
            g_engine.state = -1;
        }
        pthread_mutex_unlock(&g_engine.state_mutex);
        return NULL;
    }

    // Try native S16_LE first (Red Book CD standard: 16-bit 44.1 kHz stereo)
    bool need_s32 = false;
    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm, hw_params);
    snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);

    if (snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE) < 0) {
        // Fallback: S32_LE (supported by high-end DACs like SMSL SU-1)
        if (snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S32_LE) < 0) {
            pthread_mutex_lock(&g_engine.state_mutex);
            snprintf(g_engine.error_msg, sizeof(g_engine.error_msg), "No compatible ALSA audio format");
            g_engine.state = -1;
            pthread_mutex_unlock(&g_engine.state_mutex);
            snd_pcm_close(pcm);
            return NULL;
        }
        need_s32 = true;
    }

    snd_pcm_hw_params_set_channels(pcm, hw_params, 2);
    unsigned int rate = 44100;
    snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, 0);

    // Buffer parameters: 2048 frames period (~46ms), 16384 frames buffer (~370ms)
    snd_pcm_uframes_t buffer_frames = 16384;
    snd_pcm_uframes_t period_frames = 2048;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &buffer_frames);
    snd_pcm_hw_params_set_period_size_near(pcm, hw_params, &period_frames, 0);

    if ((err = snd_pcm_hw_params(pcm, hw_params)) < 0) {
        pthread_mutex_lock(&g_engine.state_mutex);
        snprintf(g_engine.error_msg, sizeof(g_engine.error_msg), "ALSA hw_params error: %s", snd_strerror(err));
        g_engine.state = -1;
        pthread_mutex_unlock(&g_engine.state_mutex);
        snd_pcm_close(pcm);
        return NULL;
    }

    snd_pcm_prepare(pcm);

    pthread_mutex_lock(&g_engine.state_mutex);
    g_engine.state = 1; // Playing
    g_engine.active_pcm = pcm;
    pthread_mutex_unlock(&g_engine.state_mutex);

    #define CHUNK_FRAMES 1024
    uint8_t read_buf[CHUNK_FRAMES * 4];
    int32_t s32_buf[CHUNK_FRAMES * 2];

    while (!g_engine.stop_requested) {
        if (g_engine.pause_requested) {
            snd_pcm_pause(pcm, 1);
            while (g_engine.pause_requested && !g_engine.stop_requested) {
                usleep(50000);
            }
            snd_pcm_pause(pcm, 0);
            if (g_engine.stop_requested) break;
        }

        size_t n = ring_read(&g_engine.ring, read_buf, sizeof(read_buf), &g_engine.stop_requested);
        if (n == 0) {
            if (g_engine.ring.eof) {
                if (!g_engine.stop_requested) {
                    snd_pcm_drain(pcm);
                }
                break;
            }
            usleep(10000);
            continue;
        }

        snd_pcm_uframes_t frames = n / 4;
        snd_pcm_sframes_t written = 0;

        if (need_s32) {
            // Bit-perfect shift to 32-bit container (MSB aligned: original 16 bits untouched)
            const int16_t* src16 = (const int16_t*)read_buf;
            for (size_t i = 0; i < frames * 2; i++) {
                s32_buf[i] = ((int32_t)src16[i]) << 16;
            }
            written = snd_pcm_writei(pcm, s32_buf, frames);
        } else {
            written = snd_pcm_writei(pcm, read_buf, frames);
        }

        if (written < 0) {
            if (written == -EPIPE) {
                snd_pcm_prepare(pcm);
            } else if (written == -ESTRPIPE) {
                while ((err = snd_pcm_resume(pcm)) == -EAGAIN) sleep(1);
                if (err < 0) snd_pcm_prepare(pcm);
            } else if (written == -ENODEV || written == -EIO || written == -EBADFD) {
                pthread_mutex_lock(&g_engine.state_mutex);
                snprintf(g_engine.error_msg, sizeof(g_engine.error_msg), "DAC desconectado o entrada cambiada a consola");
                g_engine.state = -3;
                pthread_mutex_unlock(&g_engine.state_mutex);
                break;
            }
        } else {
            pthread_mutex_lock(&g_engine.state_mutex);
            g_engine.frames_played += written;
            pthread_mutex_unlock(&g_engine.state_mutex);
        }
    }

    pthread_mutex_lock(&g_engine.state_mutex);
    g_engine.active_pcm = NULL;
    pthread_mutex_unlock(&g_engine.state_mutex);

    snd_pcm_close(pcm);

    pthread_mutex_lock(&g_engine.state_mutex);
    if (!g_engine.stop_requested && g_engine.state != -3) {
        g_engine.state = 0; // idle (disc completed)
    }
    pthread_mutex_unlock(&g_engine.state_mutex);

    return NULL;
}

// --- THREAD LIFECYCLE HELPERS ---

static void stop_worker_threads_locked(void) {
    if (!g_engine.threads_running) return;

    g_engine.stop_requested = true;
    g_engine.pause_requested = false;
    if (g_engine.active_pcm) {
        snd_pcm_drop(g_engine.active_pcm);
    }
    pthread_cond_broadcast(&g_engine.ring.not_empty);
    pthread_cond_broadcast(&g_engine.ring.not_full);

    // Release state_mutex while joining to prevent deadlock
    pthread_mutex_unlock(&g_engine.state_mutex);

    pthread_join(g_engine.reader_thread, NULL);
    pthread_join(g_engine.player_thread, NULL);

    pthread_mutex_lock(&g_engine.state_mutex);
    g_engine.threads_running = false;
}

// --- PUBLIC API IMPLEMENTATION ---

int cdda_set_device(const char* device_path) {
    if (!device_path || !device_path[0]) return -1;
    pthread_mutex_lock(&g_engine.state_mutex);
    strncpy(g_engine.device_path, device_path, sizeof(g_engine.device_path) - 1);
    pthread_mutex_unlock(&g_engine.state_mutex);
    return 0;
}

int cdda_init(const char* device_path, int speed_x) {
    if (g_engine.initialized) {
        cdda_stop();
        pthread_mutex_destroy(&g_engine.state_mutex);
        pthread_mutex_destroy(&g_engine.ring.mutex);
        pthread_cond_destroy(&g_engine.ring.not_empty);
        pthread_cond_destroy(&g_engine.ring.not_full);
    }

    memset(&g_engine, 0, sizeof(g_engine));

    // Must use PTHREAD_MUTEX_RECURSIVE to prevent self-deadlocks
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_engine.state_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    ring_init(&g_engine.ring);

    g_engine.cd_fd = -1;
    g_engine.speed_x = (speed_x > 0) ? speed_x : 2;
    g_engine.state = 0; // Idle
    g_engine.initialized = true;

    if (device_path && device_path[0] && access(device_path, F_OK) == 0) {
        strncpy(g_engine.device_path, device_path, sizeof(g_engine.device_path) - 1);
    } else if (access("/dev/sr0", F_OK) == 0) {
        strncpy(g_engine.device_path, "/dev/sr0", sizeof(g_engine.device_path) - 1);
    } else if (access("/dev/sr1", F_OK) == 0) {
        strncpy(g_engine.device_path, "/dev/sr1", sizeof(g_engine.device_path) - 1);
    } else {
        strncpy(g_engine.device_path, "/dev/sr0", sizeof(g_engine.device_path) - 1);
    }

    // Test device open & set hardware speed limit (whisper-quiet 2x, ~400 RPM)
    int fd = open_cd_device_with_fallback(g_engine.device_path);
    if (fd >= 0) {
        set_hardware_speed(fd, g_engine.speed_x);
        close(fd);
    }
    return 0;
}

int cdda_read_disc_info(CDDiscInfo* out_info) {
    if (!out_info) return -1;
    pthread_mutex_lock(&g_engine.state_mutex);

    int fd = g_engine.cd_fd;
    bool need_close = false;
    if (fd < 0) {
        fd = open_cd_device_with_fallback(g_engine.device_path);
        if (fd < 0) {
            pthread_mutex_unlock(&g_engine.state_mutex);
            return -1;
        }
        need_close = true;
    }

    set_hardware_speed(fd, g_engine.speed_x);

    int ret = read_disc_info_fd(fd, out_info);
    if (ret >= 0) {
        g_engine.disc_info = *out_info;
    }

    if (need_close) {
        close(fd);
    }

    pthread_mutex_unlock(&g_engine.state_mutex);
    return ret;
}

int cdda_play(int track, double seek_sec, const char* alsa_device) {
    pthread_mutex_lock(&g_engine.state_mutex);

    // Stop active playback threads WITHOUT closing cd_fd
    stop_worker_threads_locked();

    g_engine.session_id++;
    g_engine.stop_requested = false;
    g_engine.pause_requested = false;
    g_engine.frames_played = 0;
    g_engine.error_msg[0] = '\0';
    if (alsa_device && alsa_device[0]) {
        strncpy(g_engine.alsa_device, alsa_device, sizeof(g_engine.alsa_device) - 1);
    }

    // Keep spindle motor spinning: reuse open cd_fd if already valid
    if (g_engine.cd_fd < 0) {
        int fd = open_cd_device_with_fallback(g_engine.device_path);
        if (fd < 0) {
            snprintf(g_engine.error_msg, sizeof(g_engine.error_msg), "Cannot open CD drive: %s", strerror(errno));
            g_engine.state = -1;
            pthread_mutex_unlock(&g_engine.state_mutex);
            return -2;
        }
        g_engine.cd_fd = fd;
    }

    set_hardware_speed(g_engine.cd_fd, g_engine.speed_x);

    // If TOC is not yet loaded, read it
    if (g_engine.disc_info.track_count <= 0) {
        read_disc_info_fd(g_engine.cd_fd, &g_engine.disc_info);
    }

    CDDiscInfo* info = &g_engine.disc_info;
    if (info->track_count <= 0 || track < info->first_track || track > info->last_track) {
        snprintf(g_engine.error_msg, sizeof(g_engine.error_msg),
                 "Invalid track %d (trk_count=%d, first=%d, last=%d)",
                 track, info->track_count, info->first_track, info->last_track);
        g_engine.state = -1;
        pthread_mutex_unlock(&g_engine.state_mutex);
        return -1;
    }

    CDTrack* trk = &info->tracks[track - info->first_track];
    int seek_sectors = (int)(seek_sec * CD_FRAMES_PER_SEC);
    int start_lba = trk->start_lba + seek_sectors;

    g_engine.current_track = track;
    g_engine.playback_initial_lba = start_lba;
    g_engine.duration_sec = trk->duration_sec;

    g_engine.reader_track = track;
    g_engine.current_read_lba = start_lba;
    g_engine.reader_end_lba = trk->start_lba + trk->total_sectors;

    ring_reset(&g_engine.ring);

    g_engine.state = 3; // Starting
    g_engine.threads_running = true;

    pthread_create(&g_engine.reader_thread, NULL, reader_thread_func, NULL);
    pthread_create(&g_engine.player_thread, NULL, player_thread_func, NULL);

    pthread_mutex_unlock(&g_engine.state_mutex);
    return 0;
}

int cdda_pause(void) {
    pthread_mutex_lock(&g_engine.state_mutex);
    if (g_engine.state == 1) {
        g_engine.pause_requested = true;
        g_engine.state = 2; // Paused
    }
    pthread_mutex_unlock(&g_engine.state_mutex);
    return 0;
}

int cdda_resume(void) {
    pthread_mutex_lock(&g_engine.state_mutex);
    if (g_engine.state == 2) {
        g_engine.pause_requested = false;
        g_engine.state = 1; // Playing
    }
    pthread_mutex_unlock(&g_engine.state_mutex);
    return 0;
}

int cdda_stop(void) {
    pthread_mutex_lock(&g_engine.state_mutex);
    stop_worker_threads_locked();

    // Close drive descriptor on explicit stop or eject
    if (g_engine.cd_fd >= 0) {
        close(g_engine.cd_fd);
        g_engine.cd_fd = -1;
    }
    g_engine.state = 0; // Idle
    pthread_mutex_unlock(&g_engine.state_mutex);
    return 0;
}

int cdda_get_status(CDStatus* out_status) {
    if (!out_status) return -1;

    pthread_mutex_lock(&g_engine.state_mutex);
    out_status->state = g_engine.state;
    out_status->buffer_fill_pct = ring_fill_percentage(&g_engine.ring);
    snprintf(out_status->error_msg, sizeof(out_status->error_msg), "%s", g_engine.error_msg);

    if (g_engine.disc_info.track_count > 0 && g_engine.state != 0) {
        // 1 sector = 588 frames (75 sectors/sec * 588 frames = 44100 frames/sec)
        // Accurately map DAC played frames to current playing sector & track
        int current_playing_lba = g_engine.playback_initial_lba + (int)(g_engine.frames_played / 588);

        int cur_trk = g_engine.current_track;
        double elapsed = 0.0;
        double duration = g_engine.duration_sec;

        for (int i = 0; i < g_engine.disc_info.track_count; i++) {
            CDTrack* t = &g_engine.disc_info.tracks[i];
            int next_start = (i + 1 < g_engine.disc_info.track_count) ?
                             g_engine.disc_info.tracks[i+1].start_lba :
                             g_engine.disc_info.leadout_lba;

            if (current_playing_lba >= t->start_lba && current_playing_lba < next_start) {
                cur_trk = t->track_num;
                elapsed = (double)(current_playing_lba - t->start_lba) / 75.0;
                duration = t->duration_sec;
                break;
            }
        }

        if (current_playing_lba >= g_engine.disc_info.leadout_lba) {
            cur_trk = g_engine.disc_info.last_track;
            duration = g_engine.disc_info.tracks[g_engine.disc_info.track_count - 1].duration_sec;
            elapsed = duration;
        }

        g_engine.current_track = cur_trk;
        g_engine.duration_sec = duration;
        out_status->current_track = cur_trk;
        out_status->elapsed_time = elapsed;
        out_status->duration = duration;
    } else {
        out_status->current_track = g_engine.current_track;
        out_status->elapsed_time = 0.0;
        out_status->duration = g_engine.duration_sec;
    }

    pthread_mutex_unlock(&g_engine.state_mutex);
    return 0;
}

void cdda_close(void) {
    cdda_stop();
    pthread_mutex_destroy(&g_engine.state_mutex);
    pthread_mutex_destroy(&g_engine.ring.mutex);
    pthread_cond_destroy(&g_engine.ring.not_empty);
    pthread_cond_destroy(&g_engine.ring.not_full);
}
