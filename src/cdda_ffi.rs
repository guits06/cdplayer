use std::ffi::c_char;
use std::ffi::c_double;
use std::ffi::c_int;

pub const MAX_TRACKS: usize = 99;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CDTrackC {
    pub track_num: c_int,
    pub start_lba: c_int,
    pub total_sectors: c_int,
    pub duration_sec: c_int,
    pub title: [c_char; 128],
    pub artist: [c_char; 128],
}

impl Default for CDTrackC {
    fn default() -> Self {
        Self {
            track_num: 0,
            start_lba: 0,
            total_sectors: 0,
            duration_sec: 0,
            title: [0; 128],
            artist: [0; 128],
        }
    }
}

#[repr(C)]
pub struct CDDiscInfoC {
    pub album_title: [c_char; 128],
    pub album_artist: [c_char; 128],
    pub track_count: c_int,
    pub first_track: c_int,
    pub last_track: c_int,
    pub leadout_lba: c_int,
    pub tracks: [CDTrackC; MAX_TRACKS],
}

impl Default for CDDiscInfoC {
    fn default() -> Self {
        Self {
            album_title: [0; 128],
            album_artist: [0; 128],
            track_count: 0,
            first_track: 0,
            last_track: 0,
            leadout_lba: 0,
            tracks: [CDTrackC::default(); MAX_TRACKS],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CDStatusC {
    pub state: c_int,
    pub current_track: c_int,
    pub elapsed_time: c_double,
    pub duration: c_double,
    pub buffer_fill_pct: c_int,
    pub error_msg: [c_char; 256],
}

impl Default for CDStatusC {
    fn default() -> Self {
        Self {
            state: 0,
            current_track: 1,
            elapsed_time: 0.0,
            duration: 0.0,
            buffer_fill_pct: 0,
            error_msg: [0; 256],
        }
    }
}

extern "C" {
    pub fn cdda_init(device_path: *const c_char, speed_x: c_int) -> c_int;
    pub fn cdda_read_disc_info(out_info: *mut CDDiscInfoC) -> c_int;
    pub fn cdda_play(track: c_int, seek_sec: c_double, alsa_device: *const c_char) -> c_int;
    pub fn cdda_pause() -> c_int;
    pub fn cdda_resume() -> c_int;
    pub fn cdda_stop() -> c_int;
    pub fn cdda_get_status(out_status: *mut CDStatusC) -> c_int;
    pub fn cdda_set_track_title(track: c_int, title: *const c_char) -> c_int;
    pub fn cdda_set_device(device_path: *const c_char) -> c_int;
    pub fn cdda_set_channel_volume(vol_l: c_int, vol_r: c_int) -> c_int;
    pub fn cdda_set_channel_mute(mute_l: c_int, mute_r: c_int) -> c_int;
    #[allow(dead_code)]
    pub fn cdda_get_channel_volume(
        out_vol_l: *mut c_int,
        out_vol_r: *mut c_int,
        out_mute_l: *mut c_int,
        out_mute_r: *mut c_int,
    ) -> c_int;
    pub fn cdda_close();
}

pub fn c_char_to_string(buf: &[c_char]) -> String {
    let bytes: Vec<u8> = buf
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).trim().to_string()
}
