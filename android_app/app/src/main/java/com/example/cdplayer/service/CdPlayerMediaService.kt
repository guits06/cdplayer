package com.example.cdplayer.service

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.BitmapFactory
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.support.v4.media.MediaMetadataCompat
import android.support.v4.media.session.MediaSessionCompat
import android.support.v4.media.session.PlaybackStateCompat
import androidx.core.app.NotificationCompat
import com.example.cdplayer.MainActivity
import com.example.cdplayer.data.api.ApiClient
import com.example.cdplayer.data.model.PlayRequest
import com.example.cdplayer.data.model.PlayerStateResponse
import com.example.cdplayer.audio.RtpAudioStreamer
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.StateFlow

class CdPlayerMediaService : Service() {

    companion object {
        private const val CHANNEL_ID = "cd_player_media_channel"
        private const val NOTIFICATION_ID = 2002

        const val ACTION_START_RTP = "ACTION_START_RTP"
        const val ACTION_STOP_RTP = "ACTION_STOP_RTP"
        const val ACTION_PLAY = "ACTION_PLAY"
        const val ACTION_PAUSE = "ACTION_PAUSE"
        const val ACTION_PREV = "ACTION_PREV"
        const val ACTION_NEXT = "ACTION_NEXT"

        const val EXTRA_RESULT_CODE = "EXTRA_RESULT_CODE"
        const val EXTRA_RESULT_DATA = "EXTRA_RESULT_DATA"
        const val EXTRA_IP = "EXTRA_IP"
    }

    private val binder = LocalBinder()
    private val serviceScope = CoroutineScope(Dispatchers.Main + SupervisorJob())

    private var mediaSession: MediaSessionCompat? = null
    private var rtpStreamer: RtpAudioStreamer? = null
    private var mediaProjection: MediaProjection? = null
    private var targetIp: String = "192.168.1.100"

    val vuMeterLevel: StateFlow<Float>?
        get() = rtpStreamer?.vuMeterLevel

    inner class LocalBinder : Binder() {
        fun getService(): CdPlayerMediaService = this@CdPlayerMediaService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        initMediaSession()
    }

    private fun getApi() = ApiClient.getClient("http://$targetIp:8000")

    private fun initMediaSession() {
        mediaSession = MediaSessionCompat(this, "CdPlayerMediaService").apply {
            setFlags(
                MediaSessionCompat.FLAG_HANDLES_MEDIA_BUTTONS or
                        MediaSessionCompat.FLAG_HANDLES_TRANSPORT_CONTROLS
            )

            setCallback(object : MediaSessionCompat.Callback() {
                override fun onPlay() {
                    serviceScope.launch(Dispatchers.IO) {
                        try { getApi().play() } catch (_: Exception) {}
                    }
                }

                override fun onPause() {
                    serviceScope.launch(Dispatchers.IO) {
                        try { getApi().pause() } catch (_: Exception) {}
                    }
                }

                override fun onSkipToNext() {
                    serviceScope.launch(Dispatchers.IO) {
                        try { getApi().next() } catch (_: Exception) {}
                    }
                }

                override fun onSkipToPrevious() {
                    serviceScope.launch(Dispatchers.IO) {
                        try { getApi().prev() } catch (_: Exception) {}
                    }
                }

                override fun onStop() {
                    serviceScope.launch(Dispatchers.IO) {
                        try { getApi().stop() } catch (_: Exception) {}
                    }
                }
            })

            isActive = true
        }
    }

    fun updateTargetIp(ip: String) {
        if (ip.isNotEmpty()) {
            targetIp = ip
        }
    }

    fun updatePlayerState(response: PlayerStateResponse) {
        val isCdActive = response.state == "playing" || response.state == "paused"
        val isRtpActive = response.pcAudioActive || rtpStreamer != null

        if (!isCdActive && !isRtpActive) {
            // Si el reproductor no está en modo CD ni RTP, quitar notificación de foreground
            stopForeground(STOP_FOREGROUND_REMOVE)
            return
        }

        val trackInfo = response.tracks.find { it.track == response.currentTrack }
        val title = trackInfo?.title ?: if (isCdActive) "Pista ${response.currentTrack}" else "Audio PC Stream"
        val artist = trackInfo?.artist ?: if (response.albumArtist.isNotEmpty()) response.albumArtist else "Raspberry Pi CD"
        val album = response.albumTitle.ifEmpty { "Hi-Fi CD Player" }

        val metadata = MediaMetadataCompat.Builder()
            .putString(MediaMetadataCompat.METADATA_KEY_TITLE, title)
            .putString(MediaMetadataCompat.METADATA_KEY_ARTIST, artist)
            .putString(MediaMetadataCompat.METADATA_KEY_ALBUM, album)
            .putLong(MediaMetadataCompat.METADATA_KEY_DURATION, ((trackInfo?.duration ?: 0.0) * 1000).toLong())
            .build()

        mediaSession?.setMetadata(metadata)

        val playbackState = PlaybackStateCompat.Builder()
            .setActions(
                PlaybackStateCompat.ACTION_PLAY or
                        PlaybackStateCompat.ACTION_PAUSE or
                        PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS or
                        PlaybackStateCompat.ACTION_SKIP_TO_NEXT or
                        PlaybackStateCompat.ACTION_STOP
            )
            .setState(
                if (response.state == "playing") PlaybackStateCompat.STATE_PLAYING else PlaybackStateCompat.STATE_PAUSED,
                (response.elapsedTime * 1000).toLong(),
                1.0f
            )
            .build()

        mediaSession?.setPlaybackState(playbackState)

        val notification = buildMediaNotification(title, artist, response.state == "playing")
        
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val type = if (rtpStreamer != null) {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            } else {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            }
            startForeground(NOTIFICATION_ID, notification, type)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun buildMediaNotification(title: String, artist: String, isPlaying: Boolean): android.app.Notification {
        val intent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
        }
        val pendingIntent = PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val prevPendingIntent = PendingIntent.getService(
            this, 1, Intent(this, CdPlayerMediaService::class.java).apply { action = ACTION_PREV },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val playPauseAction = if (isPlaying) ACTION_PAUSE else ACTION_PLAY
        val playPauseIcon = if (isPlaying) android.R.drawable.ic_media_pause else android.R.drawable.ic_media_play
        val playPauseTitle = if (isPlaying) "Pausar" else "Reproducir"

        val playPausePendingIntent = PendingIntent.getService(
            this, 2, Intent(this, CdPlayerMediaService::class.java).apply { action = playPauseAction },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val nextPendingIntent = PendingIntent.getService(
            this, 3, Intent(this, CdPlayerMediaService::class.java).apply { action = ACTION_NEXT },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(title)
            .setContentText(artist)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(pendingIntent)
            .setOngoing(isPlaying)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .addAction(android.R.drawable.ic_media_previous, "Anterior", prevPendingIntent)
            .addAction(playPauseIcon, playPauseTitle, playPausePendingIntent)
            .addAction(android.R.drawable.ic_media_next, "Siguiente", nextPendingIntent)
            .setStyle(
                androidx.media.app.NotificationCompat.MediaStyle()
                    .setMediaSession(mediaSession?.sessionToken)
                    .setShowActionsInCompactView(0, 1, 2)
            )

        return builder.build()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_RTP -> {
                val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, -1)
                val resultData: Intent? = intent.getParcelableExtra(EXTRA_RESULT_DATA)
                val ip = intent.getStringExtra(EXTRA_IP) ?: ""

                if (resultCode != -1 || resultData == null || ip.isEmpty()) {
                    return START_NOT_STICKY
                }
                targetIp = ip

                if (rtpStreamer == null) {
                    rtpStreamer = RtpAudioStreamer()
                }

                val notif = buildMediaNotification("Audio PC (RTP)", "Transmitiendo audio a RPi", true)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    startForeground(NOTIFICATION_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
                } else {
                    startForeground(NOTIFICATION_ID, notif)
                }

                val projectionManager = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                mediaProjection = projectionManager.getMediaProjection(resultCode, resultData)

                mediaProjection?.let { projection ->
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                        projection.registerCallback(object : MediaProjection.Callback() {
                            override fun onStop() {
                                stopRtpStreaming()
                            }
                        }, android.os.Handler(android.os.Looper.getMainLooper()))
                    }

                    rtpStreamer?.startStreaming(
                        mediaProjection = projection,
                        targetIp = targetIp,
                        targetPort = 3000,
                        scope = serviceScope
                    )
                }
            }
            ACTION_STOP_RTP -> {
                stopRtpStreaming()
            }
            ACTION_PLAY -> {
                serviceScope.launch(Dispatchers.IO) {
                    try { getApi().play() } catch (_: Exception) {}
                }
            }
            ACTION_PAUSE -> {
                serviceScope.launch(Dispatchers.IO) {
                    try { getApi().pause() } catch (_: Exception) {}
                }
            }
            ACTION_PREV -> {
                serviceScope.launch(Dispatchers.IO) {
                    try { getApi().prev() } catch (_: Exception) {}
                }
            }
            ACTION_NEXT -> {
                serviceScope.launch(Dispatchers.IO) {
                    try { getApi().next() } catch (_: Exception) {}
                }
            }
        }
        return START_NOT_STICKY
    }

    fun stopRtpStreaming() {
        rtpStreamer?.stopStreaming()
        rtpStreamer = null
        mediaProjection?.stop()
        mediaProjection = null
    }

    override fun onDestroy() {
        stopRtpStreaming()
        mediaSession?.release()
        mediaSession = null
        serviceScope.cancel()
        super.onDestroy()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Reproductor de CD / Media Control",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Muestra la notificación multimedia activa para controlar el CD Player y el streaming"
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }
}
