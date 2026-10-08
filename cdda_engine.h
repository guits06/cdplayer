#ifndef CDDA_ENGINE_H
#define CDDA_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TRACKS 99
#define CD_FRAME_SIZE 2352     // 1 audio sector = 2352 bytes = 588 stereo 16-bit samples
#define CD_FRAMES_PER_SEC 75   // 75 sectors/sec = 176400 bytes/sec
#define RING_BUFFER_SECTORS 3500 // ~8.2 MB RAM buffer (~46.6 seconds of audio)

typedef struct {
    int track_num;
    int start_lba;
    int total_sectors;
    int duration_sec;
    char title[128];
    char artist[128];
} CDTrack;

typedef struct {
    char album_title[128];
    char album_artist[128];
    int track_count;
    int first_track;
    int last_track;
    int leadout_lba;
    CDTrack tracks[MAX_TRACKS];
} CDDiscInfo;

typedef struct {
    int state;             // 0: idle, 1: playing, 2: paused, 3: starting, -1: error
    int current_track;
    double elapsed_time;
    double duration;
    int buffer_fill_pct;   // 0 - 100%
    char error_msg[256];
} CDStatus;

// Public C API
int cdda_init(const char* device_path, int speed_x);
int cdda_read_disc_info(CDDiscInfo* out_info);
int cdda_play(int track, double seek_sec, const char* alsa_device);
int cdda_pause(void);
int cdda_resume(void);
int cdda_stop(void);
int cdda_get_status(CDStatus* out_status);
int cdda_set_track_title(int track, const char* title);
int cdda_set_device(const char* device_path);
void cdda_close(void);

#ifdef __cplusplus
}
#endif

#endif // CDDA_ENGINE_H
