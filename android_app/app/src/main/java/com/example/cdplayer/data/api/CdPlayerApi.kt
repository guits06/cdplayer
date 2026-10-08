package com.example.cdplayer.data.api

import com.example.cdplayer.data.model.PlayRequest
import com.example.cdplayer.data.model.PlayerStateResponse
import retrofit2.Response
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST

interface CdPlayerApi {

    @GET("api/status")
    suspend fun getStatus(): Response<PlayerStateResponse>

    @POST("api/pc_audio/enable")
    suspend fun enablePcAudio(): Response<PlayerStateResponse>

    @POST("api/pc_audio/disable")
    suspend fun disablePcAudio(): Response<PlayerStateResponse>

    @POST("api/play")
    suspend fun play(@Body request: PlayRequest = PlayRequest()): Response<PlayerStateResponse>

    @POST("api/pause")
    suspend fun pause(): Response<PlayerStateResponse>

    @POST("api/resume")
    suspend fun resume(): Response<PlayerStateResponse>

    @POST("api/stop")
    suspend fun stop(): Response<PlayerStateResponse>

    @POST("api/prev")
    suspend fun prev(): Response<PlayerStateResponse>

    @POST("api/next")
    suspend fun next(): Response<PlayerStateResponse>

    @POST("api/eject")
    suspend fun eject(): Response<PlayerStateResponse>

    @POST("api/seek")
    suspend fun seek(@Body request: Map<String, Double>): Response<PlayerStateResponse>

    @POST("api/refresh")
    suspend fun refresh(): Response<PlayerStateResponse>
}
