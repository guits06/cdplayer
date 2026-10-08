package com.example.cdplayer.ui.screen

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.cdplayer.data.model.TrackInfo
import com.example.cdplayer.ui.viewmodel.MainViewModel
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(
    viewModel: MainViewModel,
    onRequestMediaProjection: () -> Unit
) {
    val targetIp by viewModel.targetIp.collectAsState()
    val playerState by viewModel.playerState.collectAsState()
    val vuLevel by viewModel.vuLevel.collectAsState()
    val isStreamingAudio by viewModel.isStreamingAudio.collectAsState()
    val connectionError by viewModel.connectionError.collectAsState()

    val animatedVuLevel by animateFloatAsState(targetValue = vuLevel, label = "vuLevel")

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            imageVector = Icons.Rounded.Album,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.size(28.dp)
                        )
                        Spacer(modifier = Modifier.width(12.dp))
                        Text(
                            text = "RPi Hi-Fi Remote",
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onBackground
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = Color(0xFF0D0D11)
                )
            )
        },
        containerColor = Color(0xFF0D0D11)
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // 1. Sección de Conexión IP
            ConnectionCard(
                targetIp = targetIp,
                onIpChanged = viewModel::updateTargetIp,
                connectionError = connectionError
            )

            // 2. Control de Modos (CD / Audio PC)
            ModeSelectorCard(
                isPcAudioActive = playerState.pcAudioActive,
                onSelectCd = { viewModel.enableCdMode() },
                onSelectPcAudio = { viewModel.enablePcAudioMode() }
            )

            // 3. Tarjeta de Estado & VU Meter
            StatusAndVuCard(
                playerState = playerState,
                vuLevel = animatedVuLevel,
                isStreamingAudio = isStreamingAudio
            )

            // 4. Botón Principal de Transmisión (RTP / UDP)
            StreamingButton(
                isStreamingAudio = isStreamingAudio,
                onStartStreaming = onRequestMediaProjection,
                onStopStreaming = { viewModel.stopStreamingAudio() }
            )

            // 5. Lista de Pistas (Tracklist LazyColumn)
            TrackListCard(
                tracks = playerState.tracks,
                currentTrack = playerState.currentTrack,
                isPlaying = playerState.state == "playing",
                onTrackClick = { viewModel.onPlayTrackClicked(it) },
                onRefreshClick = { viewModel.onRefreshClicked() },
                modifier = Modifier.weight(1f)
            )

            // 6. Controles Remotos de Reproducción CD & Escaneo
            CdPlayerControlsCard(
                playerState = playerState,
                onPlay = { viewModel.onPlayClicked() },
                onPause = { viewModel.onPauseClicked() },
                onStop = { viewModel.onStopClicked() },
                onPrev = { viewModel.onPrevClicked() },
                onNext = { viewModel.onNextClicked() },
                onSeekBack = { viewModel.onSeekClicked(-10.0) },
                onSeekForward = { viewModel.onSeekClicked(10.0) },
                onEject = { viewModel.onEjectClicked() }
            )

            Spacer(modifier = Modifier.height(8.dp))
        }
    }
}

@Composable
fun ConnectionCard(
    targetIp: String,
    onIpChanged: (String) -> Unit,
    connectionError: String?
) {
    Card(
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xFF16161E)),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(
                    text = "RECEPTOR (RASPBERRY PI)",
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Box(
                    modifier = Modifier
                        .size(8.dp)
                        .clip(CircleShape)
                        .background(if (connectionError == null) Color(0xFF10B981) else Color(0xFFEF4444))
                )
            }

            Spacer(modifier = Modifier.height(6.dp))

            OutlinedTextField(
                value = targetIp,
                onValueChange = onIpChanged,
                label = { Text("IP de la Raspberry Pi", fontSize = 12.sp) },
                singleLine = true,
                leadingIcon = { Icon(Icons.Rounded.Router, contentDescription = null, modifier = Modifier.size(20.dp)) },
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = MaterialTheme.colorScheme.primary,
                    unfocusedBorderColor = Color(0xFF2E2E3E),
                    focusedContainerColor = Color(0xFF0F0F14),
                    unfocusedContainerColor = Color(0xFF0F0F14)
                ),
                modifier = Modifier.fillMaxWidth()
            )

            if (connectionError != null) {
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = connectionError,
                    color = MaterialTheme.colorScheme.error,
                    fontSize = 11.sp
                )
            }
        }
    }
}

@Composable
fun ModeSelectorCard(
    isPcAudioActive: Boolean,
    onSelectCd: () -> Unit,
    onSelectPcAudio: () -> Unit
) {
    Card(
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xFF16161E)),
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(6.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            val isCdSelected = !isPcAudioActive
            Button(
                onClick = onSelectCd,
                shape = RoundedCornerShape(10.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (isCdSelected) MaterialTheme.colorScheme.primary else Color(0xFF1F1F2E),
                    contentColor = if (isCdSelected) Color.White else Color.Gray
                ),
                modifier = Modifier.weight(1f)
            ) {
                Icon(Icons.Rounded.DiscFull, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(modifier = Modifier.width(6.dp))
                Text("Modo CD", fontWeight = FontWeight.Bold, fontSize = 13.sp)
            }

            Button(
                onClick = onSelectPcAudio,
                shape = RoundedCornerShape(10.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (isPcAudioActive) MaterialTheme.colorScheme.secondary else Color(0xFF1F1F2E),
                    contentColor = if (isPcAudioActive) Color.Black else Color.Gray
                ),
                modifier = Modifier.weight(1f)
            ) {
                Icon(Icons.Rounded.GraphicEq, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(modifier = Modifier.width(6.dp))
                Text("Audio PC", fontWeight = FontWeight.Bold, fontSize = 13.sp)
            }
        }
    }
}

@Composable
fun StatusAndVuCard(
    playerState: com.example.cdplayer.data.model.PlayerStateResponse,
    vuLevel: Float,
    isStreamingAudio: Boolean
) {
    Card(
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xFF16161E)),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = when {
                    isStreamingAudio -> "TRANSMITIENDO AUDIO ANDROID (RTP/UDP)"
                    playerState.pcAudioActive -> "RECEPTOR UDP LISTO (PUERTO 3000)"
                    playerState.state == "playing" -> "REPRODUCIENDO CD - PISTA ${playerState.currentTrack}"
                    playerState.state == "paused" -> "CD PAUSADO - PISTA ${playerState.currentTrack}"
                    playerState.state == "no_disc" -> "SIN DISCO EN EL LECTOR"
                    else -> "ESTADO: ${playerState.state.uppercase()}"
                },
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                color = if (isStreamingAudio) Color(0xFF10B981) else MaterialTheme.colorScheme.onSurface,
                textAlign = TextAlign.Center,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )

            Spacer(modifier = Modifier.height(10.dp))

            // VU Meter Progress Bar
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(
                    text = "VU METER",
                    fontSize = 10.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.Gray
                )
                Spacer(modifier = Modifier.width(10.dp))
                LinearProgressIndicator(
                    progress = { vuLevel },
                    color = if (vuLevel > 0.85f) Color(0xFFEF4444) else MaterialTheme.colorScheme.primary,
                    trackColor = Color(0xFF0F0F14),
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(8.dp)
                        .clip(RoundedCornerShape(4.dp))
                )
            }
        }
    }
}

@Composable
fun StreamingButton(
    isStreamingAudio: Boolean,
    onStartStreaming: () -> Unit,
    onStopStreaming: () -> Unit
) {
    Button(
        onClick = { if (isStreamingAudio) onStopStreaming() else onStartStreaming() },
        shape = RoundedCornerShape(12.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (isStreamingAudio) Color(0xFFEF4444) else Color(0xFF10B981),
            contentColor = Color.White
        ),
        modifier = Modifier
            .fillMaxWidth()
            .height(48.dp)
    ) {
        Icon(
            imageVector = if (isStreamingAudio) Icons.Rounded.StopScreenShare else Icons.Rounded.ScreenShare,
            contentDescription = null
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = if (isStreamingAudio) "DETENER TRANSMISIÓN DE AUDIO" else "TRANSMITIR AUDIO DE ESTE MÓVIL",
            fontWeight = FontWeight.Bold,
            fontSize = 13.sp
        )
    }
}

@Composable
fun TrackListCard(
    tracks: List<TrackInfo>,
    currentTrack: Int,
    isPlaying: Boolean,
    onTrackClick: (Int) -> Unit,
    onRefreshClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    Card(
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xFF16161E)),
        modifier = modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween,
                modifier = Modifier.fillMaxWidth()
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.Rounded.QueueMusic,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(18.dp)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "PISTAS DEL DISCO (${tracks.size})",
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color.White
                    )
                }

                IconButton(
                    onClick = onRefreshClick,
                    modifier = Modifier.size(28.dp)
                ) {
                    Icon(
                        imageVector = Icons.Rounded.Refresh,
                        contentDescription = "Escanear Disco",
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(20.dp)
                    )
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            if (tracks.isEmpty()) {
                Box(
                    contentAlignment = Alignment.Center,
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(16.dp)
                ) {
                    Text(
                        text = "No se han detectado pistas o el lector está vacío.",
                        color = Color.Gray,
                        fontSize = 12.sp,
                        textAlign = TextAlign.Center
                    )
                }
            } else {
                LazyColumn(
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                    modifier = Modifier.fillMaxSize()
                ) {
                    items(tracks) { track ->
                        val isCurrent = track.track == currentTrack
                        TrackItem(
                            track = track,
                            isCurrent = isCurrent,
                            isPlaying = isPlaying && isCurrent,
                            onClick = { onTrackClick(track.track) }
                        )
                    }
                }
            }
        }
    }
}

@Composable
fun TrackItem(
    track: TrackInfo,
    isCurrent: Boolean,
    isPlaying: Boolean,
    onClick: () -> Unit
) {
    val backgroundColor by animateColorAsState(
        targetValue = if (isCurrent) MaterialTheme.colorScheme.primary.copy(alpha = 0.25f) else Color(0xFF0F0F14),
        label = "trackBg"
    )

    val textColor = if (isCurrent) MaterialTheme.colorScheme.primary else Color.White

    val durationFormatted = formatDuration(track.duration)

    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(backgroundColor)
            .clickable { onClick() }
            .padding(horizontal = 12.dp, vertical = 8.dp)
    ) {
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier.width(32.dp)
        ) {
            if (isPlaying) {
                Icon(
                    imageVector = Icons.Rounded.VolumeUp,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(18.dp)
                )
            } else {
                Text(
                    text = "${track.track}",
                    fontSize = 13.sp,
                    fontWeight = if (isCurrent) FontWeight.Bold else FontWeight.Normal,
                    color = textColor
                )
            }
        }

        Spacer(modifier = Modifier.width(8.dp))

        Text(
            text = track.title ?: "Pista ${track.track}",
            fontSize = 13.sp,
            fontWeight = if (isCurrent) FontWeight.Bold else FontWeight.Normal,
            color = textColor,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f)
        )

        Text(
            text = durationFormatted,
            fontSize = 12.sp,
            color = if (isCurrent) MaterialTheme.colorScheme.primary else Color.Gray
        )
    }
}

private fun formatDuration(seconds: Double): String {
    if (seconds <= 0.0) return "--:--"
    val totalSec = seconds.toInt()
    val mins = totalSec / 60
    val secs = totalSec % 60
    return String.format(Locale.getDefault(), "%d:%02d", mins, secs)
}

@Composable
fun CdPlayerControlsCard(
    playerState: com.example.cdplayer.data.model.PlayerStateResponse,
    onPlay: () -> Unit,
    onPause: () -> Unit,
    onStop: () -> Unit,
    onPrev: () -> Unit,
    onNext: () -> Unit,
    onSeekBack: () -> Unit,
    onSeekForward: () -> Unit,
    onEject: () -> Unit
) {
    val isPlaying = playerState.state == "playing"

    Card(
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xFF16161E)),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.padding(12.dp)
        ) {
            // Fila 1: Rebobinar -10s y Avanzar +10s (Escaneo) + Expulsar
            Row(
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth()
            ) {
                IconButton(onClick = onSeekBack) {
                    Icon(
                        imageVector = Icons.Rounded.Replay10,
                        contentDescription = "-10 Segundos",
                        tint = Color.White,
                        modifier = Modifier.size(24.dp)
                    )
                }

                IconButton(onClick = onSeekForward) {
                    Icon(
                        imageVector = Icons.Rounded.Forward10,
                        contentDescription = "+10 Segundos",
                        tint = Color.White,
                        modifier = Modifier.size(24.dp)
                    )
                }

                IconButton(onClick = onEject) {
                    Icon(
                        imageVector = Icons.Rounded.Eject,
                        contentDescription = "Expulsar CD",
                        tint = Color(0xFFEF4444),
                        modifier = Modifier.size(24.dp)
                    )
                }
            }

            Spacer(modifier = Modifier.height(4.dp))

            // Fila 2: Anterior, Play/Pausa, Stop, Siguiente
            Row(
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth()
            ) {
                IconButton(onClick = onPrev) {
                    Icon(
                        imageVector = Icons.Rounded.SkipPrevious,
                        contentDescription = "Anterior",
                        tint = Color.White,
                        modifier = Modifier.size(32.dp)
                    )
                }

                FilledIconButton(
                    onClick = { if (isPlaying) onPause() else onPlay() },
                    shape = CircleShape,
                    colors = IconButtonDefaults.filledIconButtonColors(
                        containerColor = MaterialTheme.colorScheme.primary,
                        contentColor = Color.White
                    ),
                    modifier = Modifier.size(52.dp)
                ) {
                    Icon(
                        imageVector = if (isPlaying) Icons.Rounded.Pause else Icons.Rounded.PlayArrow,
                        contentDescription = if (isPlaying) "Pausar" else "Reproducir",
                        modifier = Modifier.size(32.dp)
                    )
                }

                IconButton(onClick = onStop) {
                    Icon(
                        imageVector = Icons.Rounded.Stop,
                        contentDescription = "Detener",
                        tint = Color.White,
                        modifier = Modifier.size(32.dp)
                    )
                }

                IconButton(onClick = onNext) {
                    Icon(
                        imageVector = Icons.Rounded.SkipNext,
                        contentDescription = "Siguiente",
                        tint = Color.White,
                        modifier = Modifier.size(32.dp)
                    )
                }
            }
        }
    }
}
