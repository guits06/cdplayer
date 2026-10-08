package com.example.cdplayer.audio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import kotlin.math.abs
import kotlin.random.Random

class RtpAudioStreamer {

    companion object {
        private const val TAG = "RtpAudioStreamer"
        private const val SAMPLE_RATE = 48000
        private const val CHANNELS = 2 // Estéreo
        private const val BYTES_PER_SAMPLE_OUT = 3 // L24 (24-bit)
        
        // Muestras por paquete: 480 muestras por canal (10ms a 48kHz). 
        // 480 muestras * 2 canales * 3 bytes = 2880 bytes de payload.
        // Paquete de 10ms reduce el overhead de red al 50% y estabiliza el jitter del buffer.
        private const val SAMPLES_PER_PACKET = 480
        private const val RTP_HEADER_SIZE = 12
        private const val PAYLOAD_TYPE = 96
    }

    private var audioRecord: AudioRecord? = null
    private var socket: DatagramSocket? = null
    private var streamJob: Job? = null

    // VU Meter Level (0.0f a 1.0f)
    private val _vuMeterLevel = MutableStateFlow(0.0f)
    val vuMeterLevel: StateFlow<Float> = _vuMeterLevel.asStateFlow()

    // Encabezado RTP
    private var sequenceNumber: Short = Random.nextInt(0, 65535).toShort()
    private var timestamp: Int = Random.nextInt()
    private val ssrc: Int = Random.nextInt()

    @SuppressLint("MissingPermission")
    fun startStreaming(
        mediaProjection: MediaProjection,
        targetIp: String,
        targetPort: Int = 3000,
        scope: CoroutineScope
    ) {
        if (streamJob?.isActive == true) return

        streamJob = scope.launch(Dispatchers.IO) {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)

            val minBufferSize = AudioRecord.getMinBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_STEREO,
                AudioFormat.ENCODING_PCM_16BIT
            )

            val config = AudioPlaybackCaptureConfiguration.Builder(mediaProjection)
                .addMatchingUsage(android.media.AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(android.media.AudioAttributes.USAGE_GAME)
                .addMatchingUsage(android.media.AudioAttributes.USAGE_UNKNOWN)
                .build()

            // Buffer holgado de AudioRecord (4x minBufferSize) para evitar underruns internos de Android
            audioRecord = AudioRecord.Builder()
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                        .build()
                )
                .setBufferSizeInBytes(minBufferSize * 4)
                .setAudioPlaybackCaptureConfig(config)
                .build()

            try {
                socket = DatagramSocket().apply {
                    sendBufferSize = 1024 * 1024 // 1 MB Send Buffer para suavizar picos de red
                }
                val targetAddress = InetAddress.getByName(targetIp)

                audioRecord?.startRecording()
                Log.i(TAG, "AudioCapture & RTP Streamer optimizado iniciado a $targetIp:$targetPort")

                val inputBufferSamples = SAMPLES_PER_PACKET * CHANNELS
                val audioShortBuffer = ShortArray(inputBufferSamples)
                val rtpPacketBuffer = ByteArray(RTP_HEADER_SIZE + (SAMPLES_PER_PACKET * CHANNELS * BYTES_PER_SAMPLE_OUT))

                while (isActive) {
                    val samplesRead = audioRecord?.read(audioShortBuffer, 0, inputBufferSamples, AudioRecord.READ_BLOCKING) ?: -1
                    if (samplesRead <= 0) continue

                    // 1. Nivel VU Meter (Peak Amplitude / Max 32768)
                    var maxAmp = 0
                    for (i in 0 until samplesRead) {
                        val absVal = abs(audioShortBuffer[i].toInt())
                        if (absVal > maxAmp) maxAmp = absVal
                    }
                    _vuMeterLevel.value = (maxAmp / 32768.0f).coerceIn(0.0f, 1.0f)

                    // 2. Construir Encabezado RTP (12 Bytes)
                    buildRtpHeader(rtpPacketBuffer, sequenceNumber, timestamp, ssrc)

                    // 3. Convertir PCM 16-bit (Little-Endian) a PCM 24-bit (Big-Endian / L24)
                    var outIndex = RTP_HEADER_SIZE
                    for (i in 0 until samplesRead) {
                        val sample16 = audioShortBuffer[i].toInt()
                        val sample24 = sample16 shl 8

                        rtpPacketBuffer[outIndex++] = (sample24 shr 16 and 0xFF).toByte()
                        rtpPacketBuffer[outIndex++] = (sample24 shr 8 and 0xFF).toByte()
                        rtpPacketBuffer[outIndex++] = (sample24 and 0xFF).toByte()
                    }

                    // 4. Enviar Datagrama UDP
                    val packetLength = RTP_HEADER_SIZE + (samplesRead * BYTES_PER_SAMPLE_OUT)
                    val packet = DatagramPacket(rtpPacketBuffer, packetLength, targetAddress, targetPort)
                    socket?.send(packet)

                    // 5. Incrementar Secuencia y Timestamp RTP
                    sequenceNumber++
                    val framesSent = samplesRead / CHANNELS
                    timestamp += framesSent
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error en el bucle de streaming RTP: ${e.message}", e)
            } finally {
                stopInternal()
            }
        }
    }

    private fun buildRtpHeader(buffer: ByteArray, seqNum: Short, ts: Int, ssrcVal: Int) {
        buffer[0] = 0x80.toByte() // V=2, P=0, X=0, CC=0
        buffer[1] = (PAYLOAD_TYPE and 0x7F).toByte() // Dynamic Payload Type 96

        buffer[2] = (seqNum.toInt() shr 8 and 0xFF).toByte()
        buffer[3] = (seqNum.toInt() and 0xFF).toByte()

        buffer[4] = (ts shr 24 and 0xFF).toByte()
        buffer[5] = (ts shr 16 and 0xFF).toByte()
        buffer[6] = (ts shr 8 and 0xFF).toByte()
        buffer[7] = (ts and 0xFF).toByte()

        buffer[8] = (ssrcVal shr 24 and 0xFF).toByte()
        buffer[9] = (ssrcVal shr 16 and 0xFF).toByte()
        buffer[10] = (ssrcVal shr 8 and 0xFF).toByte()
        buffer[11] = (ssrcVal and 0xFF).toByte()
    }

    fun stopStreaming() {
        streamJob?.cancel()
        streamJob = null
        stopInternal()
    }

    private fun stopInternal() {
        try {
            audioRecord?.stop()
            audioRecord?.release()
        } catch (e: Exception) {
            Log.e(TAG, "Error liberando AudioRecord: ${e.message}")
        }
        audioRecord = null

        try {
            socket?.close()
        } catch (e: Exception) {
            Log.e(TAG, "Error cerrando DatagramSocket: ${e.message}")
        }
        socket = null
        _vuMeterLevel.value = 0.0f
    }
}
