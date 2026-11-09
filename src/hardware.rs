use crate::cdda_ffi;
use crate::state::AudioInterface;
use glob::glob;
use std::fs;
use std::os::unix::fs::MetadataExt;
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::thread;
use std::time::Duration;

pub const CDROM_SELECT_SPEED: libc::c_ulong = 0x5322;
pub const CDROM_DRIVE_STATUS: libc::c_ulong = 0x5326;
pub const CDROM_LOCKDOOR: libc::c_ulong = 0x5329;

pub const CDS_NO_DISC: libc::c_int = 1;
pub const CDS_TRAY_OPEN: libc::c_int = 2;
pub const CDS_DRIVE_NOT_READY: libc::c_int = 3;
pub const CDS_DISC_OK: libc::c_int = 4;

pub fn get_cd_device() -> String {
    // 1. Check persistent USB optical drive symlinks
    let patterns = ["/dev/disk/by-id/usb-*DVD*", "/dev/disk/by-id/usb-*CD*"];
    for pat in &patterns {
        if let Ok(entries) = glob(pat) {
            for entry in entries.flatten() {
                if let Ok(real) = fs::canonicalize(&entry) {
                    return real.to_string_lossy().to_string();
                }
            }
        }
    }

    // 2. Search /dev/sr* devices and select the newest mtime
    if let Ok(entries) = glob("/dev/sr*") {
        let mut devices: Vec<PathBuf> = entries.flatten().collect();
        devices.sort_by(|a, b| {
            let mtime_a = fs::metadata(a).map(|m| m.mtime()).unwrap_or(0);
            let mtime_b = fs::metadata(b).map(|m| m.mtime()).unwrap_or(0);
            mtime_b.cmp(&mtime_a)
        });
        if let Some(first) = devices.first() {
            if let Ok(real) = fs::canonicalize(first) {
                return real.to_string_lossy().to_string();
            }
        }
    }

    "/dev/sr0".to_string()
}

pub fn set_drive_speed(speed: i32) {
    let dev = get_cd_device();
    if !Path::new(&dev).exists() {
        return;
    }

    // 1. Direct Linux CD-ROM ioctl (unlock manual door button + clamp speed)
    if let Ok(file) = fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(&dev)
    {
        let fd = file.as_raw_fd();
        unsafe {
            libc::ioctl(fd, CDROM_LOCKDOOR, 0 as libc::c_ulong);
            libc::ioctl(fd, CDROM_SELECT_SPEED, speed as libc::c_ulong);
        }
        return;
    }

    // 2. Userspace fallback
    let _ = Command::new("setcd")
        .args(["-x", &speed.to_string(), &dev])
        .output();
}

pub fn check_cd_inserted(current_presence: bool) -> bool {
    let dev = get_cd_device();
    if !Path::new(&dev).exists() {
        return false;
    }

    // 1. Fast non-blocking kernel ioctl (< 1ms)
    if let Ok(file) = fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(&dev)
    {
        let fd = file.as_raw_fd();
        let st = unsafe { libc::ioctl(fd, CDROM_DRIVE_STATUS, 0 as libc::c_ulong) };
        if st == CDS_DISC_OK {
            return true;
        }
        if st == CDS_NO_DISC || st == CDS_TRAY_OPEN {
            if current_presence {
                // Debounce transient bus glitch before confirming disc ejection
                thread::sleep(Duration::from_millis(100));
                let st2 = unsafe { libc::ioctl(fd, CDROM_DRIVE_STATUS, 0 as libc::c_ulong) };
                if st2 == CDS_DISC_OK {
                    return true;
                }
            }
            return false;
        }
        if st == CDS_DRIVE_NOT_READY {
            return current_presence;
        }
    }

    // 2. Fallback to setcd -i
    if let Ok(out) = Command::new("setcd").args(["-i", &dev]).output() {
        let combined = format!(
            "{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        )
        .to_lowercase();
        if combined.contains("disc found") {
            return true;
        }
        if combined.contains("no disc is inserted") || combined.contains("tray is open") {
            return false;
        }
        if combined.contains("drive is not ready") {
            return current_presence;
        }
    }

    current_presence
}

pub fn get_audio_interfaces() -> Vec<AudioInterface> {
    let mut interfaces = vec![AudioInterface {
        id: "default".to_string(),
        name: "Salida por Defecto (ALSA)".to_string(),
    }];

    if let Ok(content) = fs::read_to_string("/proc/asound/cards") {
        let re = regex::Regex::new(r"(?m)^\s*(\d+)\s*\[(\w+)\s*\]:\s*(.*?)$").unwrap();
        for cap in re.captures_iter(&content) {
            let card_id = cap[2].to_string();
            let card_desc = cap[3].trim();
            let card_name = if let Some(idx) = card_desc.rfind(" - ") {
                &card_desc[idx + 3..]
            } else {
                card_desc
            };
            interfaces.push(AudioInterface {
                id: card_id.clone(),
                name: format!("{} ({})", card_name, card_id),
            });
        }
    }

    interfaces
}

pub fn detect_usb_dac(manual_dac: bool, manual_dac_name: &str) -> String {
    if manual_dac {
        let interfaces = get_audio_interfaces();
        if interfaces.iter().any(|i| i.id == manual_dac_name) {
            return manual_dac_name.to_string();
        }
    }

    if let Ok(content) = fs::read_to_string("/proc/asound/cards") {
        let re_usb = regex::Regex::new(r"(?m)^\s*(\d+)\s*\[(\w+)\s*\]:\s*USB-Audio").unwrap();
        if let Some(cap) = re_usb.captures(&content) {
            return cap[2].to_string();
        }

        let re_smsl =
            regex::Regex::new(r"(?mi)^\s*(\d+)\s*\[(\w+)\s*\]:\s*.*(?:SMSL|AUDIO)").unwrap();
        if let Some(cap) = re_smsl.captures(&content) {
            return cap[2].to_string();
        }
    }

    "disconnected".to_string()
}

pub fn sync_external_volume(vol_l: i32, vol_r: i32, mute_l: bool, mute_r: bool, dac_name: &str) {
    let eff_l = if mute_l { 0 } else { vol_l };
    let eff_r = if mute_r { 0 } else { vol_r };

    // 1. Sync to native C engine if playing CD
    unsafe {
        cdda_ffi::cdda_set_channel_volume(eff_l, eff_r);
        cdda_ffi::cdda_set_channel_mute(if mute_l { 1 } else { 0 }, if mute_r { 1 } else { 0 });
    }

    // 2. Sync to ALSA softvol controls (Stream and Bluetooth)
    let card = if dac_name != "default" && dac_name != "disconnected" {
        dac_name
    } else {
        "AUDIO"
    };

    let stream_val = format!("{}%,{}%", eff_l, eff_r);

    let res = Command::new("amixer")
        .args(["-c", card, "set", "Stream", &stream_val])
        .output();

    if let Ok(o) = res {
        if !o.status.success() {
            let _ = Command::new("alsactl").arg("restore").output();
            let _ = Command::new("amixer")
                .args(["-c", card, "set", "Stream", &stream_val])
                .output();
        }
    }

    let _ = Command::new("amixer")
        .args(["-c", card, "set", "Bluetooth", &stream_val])
        .output();
}
