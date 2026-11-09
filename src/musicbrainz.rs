use serde::{Deserialize, Serialize};
use std::collections::HashMap;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MbTrackInfo {
    pub track: i32,
    pub title: String,
    pub artist: String,
    pub duration: i32,
    pub formatted_duration: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MbReleaseInfo {
    pub id: String,
    pub title: String,
    pub artist: String,
    pub year: String,
    pub cover_url: String,
    pub disc_number: i32,
    pub disc_total: i32,
    pub duration_diff: i32,
    pub tracks: Vec<MbTrackInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MusicBrainzResponse {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    pub releases: Vec<MbReleaseInfo>,
}

// Internal deserialization structures for MusicBrainz API
#[derive(Deserialize)]
struct RawMbRecording {
    title: Option<String>,
    length: Option<u64>,
    #[serde(rename = "artist-credit")]
    artist_credit: Option<Vec<RawArtistCredit>>,
}

#[derive(Deserialize)]
struct RawArtistCredit {
    name: Option<String>,
    artist: Option<RawArtist>,
}

#[derive(Deserialize)]
struct RawArtist {
    name: Option<String>,
}

#[derive(Deserialize)]
struct RawMbTrack {
    number: Option<String>,
    title: Option<String>,
    length: Option<u64>,
    recording: Option<RawMbRecording>,
}

#[derive(Deserialize)]
struct RawMbMedium {
    position: Option<i32>,
    tracks: Option<Vec<RawMbTrack>>,
}

#[derive(Deserialize)]
struct RawMbRelease {
    id: Option<String>,
    title: Option<String>,
    date: Option<String>,
    #[serde(rename = "artist-credit")]
    artist_credit: Option<Vec<RawArtistCredit>>,
    media: Option<Vec<RawMbMedium>>,
}

#[derive(Deserialize)]
struct RawMbRoot {
    releases: Option<Vec<RawMbRelease>>,
}

pub async fn query_musicbrainz(
    offsets: &[(i32, i32)],
    leadout: i32,
    actual_durations: &HashMap<i32, i32>,
) -> MusicBrainzResponse {
    if offsets.is_empty() {
        return MusicBrainzResponse {
            error: Some("No hay datos TOC en memoria. Escanea el CD primero.".to_string()),
            releases: Vec::new(),
        };
    }

    let first_track = offsets[0].0;
    let last_track = offsets[offsets.len() - 1].0;
    let expected_track_count = offsets.len();

    let mb_offsets: Vec<String> = offsets.iter().map(|o| (o.1 + 150).to_string()).collect();
    let mb_leadout = if leadout > 0 {
        leadout + 150
    } else {
        offsets[offsets.len() - 1].1 + 150 + 300
    };

    let toc_str = format!(
        "{}+{}+{}+{}",
        first_track,
        last_track,
        mb_leadout,
        mb_offsets.join("+")
    );
    let url = format!(
        "https://musicbrainz.org/ws/2/discid/-?toc={}&fmt=json&inc=artist-credits+recordings",
        urlencoding::encode(&toc_str)
    );

    let client = reqwest::Client::builder()
        .user_agent("RpiCdPlayer/1.0 (https://github.com/guillewan2/cdplayer)")
        .timeout(std::time::Duration::from_secs(12))
        .build();

    let client = match client {
        Ok(c) => c,
        Err(e) => {
            return MusicBrainzResponse {
                error: Some(format!("Error inicializando cliente HTTP: {}", e)),
                releases: Vec::new(),
            }
        }
    };

    let mut last_err = String::new();
    for attempt in 0..2 {
        if attempt > 0 {
            tokio::time::sleep(std::time::Duration::from_millis(1200)).await;
        }

        match client
            .get(&url)
            .header("Accept", "application/json")
            .send()
            .await
        {
            Ok(resp) => {
                if resp.status().is_success() {
                    match resp.json::<RawMbRoot>().await {
                        Ok(data) => {
                            let releases_raw = data.releases.unwrap_or_default();
                            if releases_raw.is_empty() {
                                return MusicBrainzResponse {
                                    error: Some("No se encontró el disco en MusicBrainz.".to_string()),
                                    releases: Vec::new(),
                                };
                            }

                            let mut results = Vec::new();
                            for rel in releases_raw {
                                let rel_id = rel.id.unwrap_or_default();
                                let title = rel.title.unwrap_or_else(|| "Desconocido".to_string());
                                let date_year = rel
                                    .date
                                    .as_ref()
                                    .and_then(|d| d.chars().take(4).collect::<String>().into())
                                    .unwrap_or_default();

                                let mut artist = String::new();
                                if let Some(ac) = rel.artist_credit.as_ref() {
                                    if let Some(first_ac) = ac.first() {
                                        artist = first_ac
                                            .name
                                            .clone()
                                            .or_else(|| {
                                                first_ac.artist.as_ref().and_then(|a| a.name.clone())
                                            })
                                            .unwrap_or_default();
                                    }
                                }

                                let all_media = rel.media.unwrap_or_default();
                                let disc_total = all_media.len() as i32;

                                let mut matched_medium: Option<RawMbMedium> = None;
                                let mut disc_number = 1;

                                for medium in &all_media {
                                    if let Some(ref m_tracks) = medium.tracks {
                                        if m_tracks.len() == expected_track_count {
                                            matched_medium = Some(medium.clone());
                                            disc_number = medium.position.unwrap_or(1);
                                            break;
                                        }
                                    }
                                }
                                if matched_medium.is_none() {
                                    for medium in &all_media {
                                        if medium.tracks.is_some() {
                                            matched_medium = Some(medium.clone());
                                            disc_number = medium.position.unwrap_or(1);
                                            break;
                                        }
                                    }
                                }

                                let mut track_list = Vec::new();
                                let mut duration_diff = 0;

                                if let Some(medium) = matched_medium {
                                    let m_tracks = medium.tracks.unwrap_or_default();
                                    if m_tracks.len() != expected_track_count {
                                        duration_diff += 5000
                                            * (m_tracks.len() as i32 - expected_track_count as i32)
                                                .abs();
                                    }

                                    for tr in m_tracks {
                                        let tn_int = tr
                                            .number
                                            .as_ref()
                                            .and_then(|n| n.parse::<i32>().ok())
                                            .unwrap_or((track_list.len() + 1) as i32);

                                        let rec = tr.recording.as_ref();
                                        let tr_title = rec
                                            .and_then(|r| r.title.clone())
                                            .or(tr.title.clone())
                                            .unwrap_or_else(|| format!("Pista {}", tn_int));

                                        let mut tr_artist = String::new();
                                        if let Some(r) = rec {
                                            if let Some(r_ac) = r.artist_credit.as_ref() {
                                                if let Some(first_r_ac) = r_ac.first() {
                                                    tr_artist = first_r_ac
                                                        .name
                                                        .clone()
                                                        .or_else(|| {
                                                            first_r_ac
                                                                .artist
                                                                .as_ref()
                                                                .and_then(|a| a.name.clone())
                                                        })
                                                        .unwrap_or_default();
                                                }
                                            }
                                        }

                                        let tr_len_ms = rec
                                            .and_then(|r| r.length)
                                            .or(tr.length)
                                            .unwrap_or(0);
                                        let tr_dur = (tr_len_ms / 1000) as i32;

                                        if let Some(&actual_dur) = actual_durations.get(&tn_int) {
                                            if tr_dur > 0 {
                                                duration_diff += (actual_dur - tr_dur).abs();
                                            } else {
                                                duration_diff += 30;
                                            }
                                        } else {
                                            duration_diff += 30;
                                        }

                                        let mins = tr_dur / 60;
                                        let secs = tr_dur % 60;

                                        track_list.push(MbTrackInfo {
                                            track: tn_int,
                                            title: tr_title,
                                            artist: if tr_artist != artist {
                                                tr_artist
                                            } else {
                                                String::new()
                                            },
                                            duration: tr_dur,
                                            formatted_duration: format!("{:02}:{:02}", mins, secs),
                                        });
                                    }
                                } else {
                                    duration_diff = 99999;
                                }

                                let cover_url = if !rel_id.is_empty() {
                                    format!("https://coverartarchive.org/release/{}/front-250", rel_id)
                                } else {
                                    String::new()
                                };

                                results.push(MbReleaseInfo {
                                    id: rel_id,
                                    title,
                                    artist,
                                    year: date_year,
                                    cover_url,
                                    disc_number,
                                    disc_total,
                                    duration_diff,
                                    tracks: track_list,
                                });
                            }

                            results.sort_by_key(|r| r.duration_diff);
                            if results.len() > 10 {
                                results.truncate(10);
                            }

                            return MusicBrainzResponse {
                                error: None,
                                releases: results,
                            };
                        }
                        Err(e) => last_err = format!("Error parseando JSON de MusicBrainz: {}", e),
                    }
                } else {
                    last_err = format!("MusicBrainz status {}", resp.status());
                }
            }
            Err(e) => last_err = format!("Error consultando MusicBrainz: {}", e),
        }
    }

    MusicBrainzResponse {
        error: Some(last_err),
        releases: Vec::new(),
    }
}

// Module for simple urlencoding
mod urlencoding {
    pub fn encode(data: &str) -> String {
        let mut result = String::with_capacity(data.len());
        for byte in data.bytes() {
            match byte {
                b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                    result.push(byte as char);
                }
                b'+' => {
                    result.push_str("%2B");
                }
                _ => {
                    result.push_str(&format!("%{:02X}", byte));
                }
            }
        }
        result
    }
}

impl Clone for RawMbMedium {
    fn clone(&self) -> Self {
        Self {
            position: self.position,
            tracks: self.tracks.as_ref().map(|v| {
                v.iter()
                    .map(|t| RawMbTrack {
                        number: t.number.clone(),
                        title: t.title.clone(),
                        length: t.length,
                        recording: t.recording.as_ref().map(|r| RawMbRecording {
                            title: r.title.clone(),
                            length: r.length,
                            artist_credit: r.artist_credit.as_ref().map(|ac| {
                                ac.iter()
                                    .map(|a| RawArtistCredit {
                                        name: a.name.clone(),
                                        artist: a.artist.as_ref().map(|art| RawArtist {
                                            name: art.name.clone(),
                                        }),
                                    })
                                    .collect()
                            }),
                        }),
                    })
                    .collect()
            }),
        }
    }
}
