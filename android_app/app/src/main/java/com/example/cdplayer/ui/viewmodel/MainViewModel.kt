package com.example.cdplayer.ui.viewmodel

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.example.cdplayer.data.api.ApiClient
import com.example.cdplayer.data.model.PlayerStateResponse
import com.example.cdplayer.service.CdPlayerMediaService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainViewModel(application: Application) : AndroidViewModel(application) {

    private val _targetIp = MutableStateFlow("192.168.1.100")
    val targetIp: StateFlow<String> = _targetIp.asStateFlow()

    private val _playerState = MutableStateFlow(PlayerStateResponse())
    val playerState: StateFlow<PlayerStateResponse> = _playerState.asStateFlow()

    private val _vuLevel = MutableStateFlow(0.0f)
    val vuLevel: StateFlow<Float> = _vuLevel.asStateFlow()

    private val _isStreamingAudio = MutableStateFlow(false)
    val isStreamingAudio: StateFlow<Boolean> = _isStreamingAudio.asStateFlow()

    private val _connectionError = MutableStateFlow<String?>(null)
    val connectionError: StateFlow<String?> = _connectionError.asStateFlow()

    private var mediaService: CdPlayerMediaService? = null
    private var serviceBound = false

    private var pollingJob: Job? = null
    private var vuJob: Job? = null

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as CdPlayerMediaService.LocalBinder
            mediaService = binder.getService()
            serviceBound = true

            mediaService?.updateTargetIp(_targetIp.value)
            mediaService?.updatePlayerState(_playerState.value)

            vuJob?.cancel()
            vuJob = viewModelScope.launch {
                mediaService?.vuMeterLevel?.collect { level ->
                    _vuLevel.value = level
                }
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            mediaService = null
            serviceBound = false
            _vuLevel.value = 0.0f
        }
    }

    init {
        bindMediaService()
        startStatusPolling()
    }

    private fun bindMediaService() {
        val app = getApplication<Application>()
        val intent = Intent(app, CdPlayerMediaService::class.java)
        app.bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
    }

    fun updateTargetIp(ip: String) {
        _targetIp.value = ip
        mediaService?.updateTargetIp(ip)
    }

    private fun getApi() = ApiClient.getClient("http://${_targetIp.value}:8000")

    private fun startStatusPolling() {
        pollingJob?.cancel()
        pollingJob = viewModelScope.launch(Dispatchers.IO) {
            while (isActive) {
                try {
                    val response = getApi().getStatus()
                    if (response.isSuccessful && response.body() != null) {
                        val stateBody = response.body()!!
                        _playerState.value = stateBody
                        _connectionError.value = null
                        withContext(Dispatchers.Main) {
                            mediaService?.updatePlayerState(stateBody)
                        }
                    } else {
                        _connectionError.value = "Error ${response.code()}: ${response.message()}"
                    }
                } catch (e: Exception) {
                    _connectionError.value = "Sin conexión con RPi (${e.localizedMessage})"
                }
                delay(2000)
            }
        }
    }

    fun enableCdMode() {
        viewModelScope.launch(Dispatchers.IO) {
            if (_isStreamingAudio.value) {
                stopStreamingAudio()
            }
            try {
                getApi().disablePcAudio()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun enablePcAudioMode() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().enablePcAudio()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun startStreamingAudio(resultCode: Int, data: Intent) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().enablePcAudio()

                val app = getApplication<Application>()
                val serviceIntent = Intent(app, CdPlayerMediaService::class.java).apply {
                    action = CdPlayerMediaService.ACTION_START_RTP
                    putExtra(CdPlayerMediaService.EXTRA_RESULT_CODE, resultCode)
                    putExtra(CdPlayerMediaService.EXTRA_RESULT_DATA, data)
                    putExtra(CdPlayerMediaService.EXTRA_IP, _targetIp.value)
                }

                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                    app.startForegroundService(serviceIntent)
                } else {
                    app.startService(serviceIntent)
                }

                _isStreamingAudio.value = true
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = "Fallo al iniciar streaming: ${e.localizedMessage}"
            }
        }
    }

    fun stopStreamingAudio() {
        viewModelScope.launch(Dispatchers.IO) {
            val app = getApplication<Application>()
            val serviceIntent = Intent(app, CdPlayerMediaService::class.java).apply {
                action = CdPlayerMediaService.ACTION_STOP_RTP
            }
            app.startService(serviceIntent)

            _isStreamingAudio.value = false
            _vuLevel.value = 0.0f

            try {
                getApi().disablePcAudio()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onPlayClicked() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().play()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onPlayTrackClicked(trackNumber: Int) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().play(com.example.cdplayer.data.model.PlayRequest(track = trackNumber))
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onPauseClicked() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().pause()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onStopClicked() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().stop()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onPrevClicked() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().prev()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onNextClicked() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().next()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onSeekClicked(offsetSeconds: Double) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val currentPos = _playerState.value.elapsedTime
                val newPos = (currentPos + offsetSeconds).coerceAtLeast(0.0)
                getApi().seek(mapOf("position" to newPos))
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onRefreshClicked() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().refresh()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    fun onEjectClicked() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                getApi().eject()
                fetchStatusNow()
            } catch (e: Exception) {
                _connectionError.value = e.localizedMessage
            }
        }
    }

    private suspend fun fetchStatusNow() {
        try {
            val response = getApi().getStatus()
            if (response.isSuccessful && response.body() != null) {
                val stateBody = response.body()!!
                _playerState.value = stateBody
                withContext(Dispatchers.Main) {
                    mediaService?.updatePlayerState(stateBody)
                }
            }
        } catch (e: Exception) {
            // Ignorar errores puntuales
        }
    }

    override fun onCleared() {
        if (serviceBound) {
            getApplication<Application>().unbindService(serviceConnection)
            serviceBound = false
        }
        super.onCleared()
    }
}
