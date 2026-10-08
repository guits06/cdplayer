package com.example.cdplayer.data.model

import com.google.gson.annotations.SerializedName

data class TrackInfo(
    @SerializedName("track") val track: Int,
    @SerializedName("duration") val duration: Double = 0.0,
    @SerializedName("title") val title: String? = null,
    @SerializedName("artist") val artist: String? = null
)

data class PlayerStateResponse(
    @SerializedName("state") val state: String = "idle",
    @SerializedName("current_track") val currentTrack: Int = 1,
    @SerializedName("tracks") val tracks: List<TrackInfo> = emptyList(),
    @SerializedName("dac_name") val dacName: String = "default",
    @SerializedName("elapsed_time") val elapsedTime: Double = 0.0,
    @SerializedName("error_message") val errorMessage: String = "",
    @SerializedName("pc_audio_active") val pcAudioActive: Boolean = false,
    @SerializedName("buffer_ms") val bufferMs: Int = 2500,
    @SerializedName("album_title") val albumTitle: String = "",
    @SerializedName("album_artist") val albumArtist: String = "",
    @SerializedName("cover_url") val coverUrl: String = ""
)

data class PlayRequest(
    @SerializedName("track") val track: Int = 1
)

data class TrackSelectRequest(
    @SerializedName("track") val track: Int
)
