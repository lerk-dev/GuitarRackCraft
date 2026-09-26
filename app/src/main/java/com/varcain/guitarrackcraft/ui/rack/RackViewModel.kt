/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

package com.varcain.guitarrackcraft.ui.rack

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.varcain.guitarrackcraft.R
import com.varcain.guitarrackcraft.engine.AudioEngine
import com.varcain.guitarrackcraft.engine.AudioForegroundService
import com.varcain.guitarrackcraft.engine.LanguageManager
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.guitarrackcraft.engine.PresetManager
import com.varcain.guitarrackcraft.engine.RackManager
import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.PluginUiPreferenceManager
import com.varcain.guitarrackcraft.engine.RecordingManager
import com.varcain.guitarrackcraft.engine.RecentPresetsManager
import com.varcain.guitarrackcraft.engine.UiType
import com.varcain.guitarrackcraft.engine.WavPlayer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class RackPlugin(
    val index: Int,
    val name: String,
    val pluginId: String,
    val instanceId: Long = nextInstanceId()
) {
    companion object {
        private val counter = java.util.concurrent.atomic.AtomicLong(0)
        fun nextInstanceId(): Long = counter.getAndIncrement()
    }
}

class RackViewModel(application: Application) : AndroidViewModel(application) {

    /** 按当前应用语言返回包装后的上下文，用于解析本地化字符串。 */
    private fun ctx(): Context = LanguageManager.wrapContext(getApplication())

    private val _isEngineRunning = MutableStateFlow(false)
    val isEngineRunning: StateFlow<Boolean> = _isEngineRunning.asStateFlow()

    private val _latencyMs = MutableStateFlow(0.0)
    val latencyMs: StateFlow<Double> = _latencyMs.asStateFlow()

    private val _inputLevel = MutableStateFlow(0f)
    val inputLevel: StateFlow<Float> = _inputLevel.asStateFlow()

    private val _outputLevel = MutableStateFlow(0f)
    val outputLevel: StateFlow<Float> = _outputLevel.asStateFlow()

    private val _cpuLoad = MutableStateFlow(0f)
    val cpuLoad: StateFlow<Float> = _cpuLoad.asStateFlow()

    private val _xRunCount = MutableStateFlow(0)
    val xRunCount: StateFlow<Int> = _xRunCount.asStateFlow()

    private val _inputClipping = MutableStateFlow(false)
    val inputClipping: StateFlow<Boolean> = _inputClipping.asStateFlow()

    private val _outputClipping = MutableStateFlow(false)
    val outputClipping: StateFlow<Boolean> = _outputClipping.asStateFlow()

    private val _rackPlugins = MutableStateFlow<List<RackPlugin>>(emptyList())
    val rackPlugins: StateFlow<List<RackPlugin>> = _rackPlugins.asStateFlow()

    private val _errorMessage = MutableStateFlow<String?>(null)
    val errorMessage: StateFlow<String?> = _errorMessage.asStateFlow()

    // Preset state
    private val presetManager = PresetManager(NativeEngine.getInstance())
    private var recentPresetsManager: RecentPresetsManager? = null

    private val _presetList = MutableStateFlow<List<String>>(emptyList())
    val presetList: StateFlow<List<String>> = _presetList.asStateFlow()

    private val _recentPresets = MutableStateFlow<List<String>>(emptyList())
    val recentPresets: StateFlow<List<String>> = _recentPresets.asStateFlow()

    private val _presetMessage = MutableStateFlow<String?>(null)
    val presetMessage: StateFlow<String?> = _presetMessage.asStateFlow()

    private val _blockingOperation = MutableStateFlow<String?>(null)
    val blockingOperation: StateFlow<String?> = _blockingOperation.asStateFlow()

    // WAV playback state
    private val _wavLoaded = MutableStateFlow(false)
    val wavLoaded: StateFlow<Boolean> = _wavLoaded.asStateFlow()

    private val _wavDurationSec = MutableStateFlow(0.0)
    val wavDurationSec: StateFlow<Double> = _wavDurationSec.asStateFlow()

    private val _wavPositionSec = MutableStateFlow(0.0)
    val wavPositionSec: StateFlow<Double> = _wavPositionSec.asStateFlow()

    private val _isWavPlaying = MutableStateFlow(false)
    val isWavPlaying: StateFlow<Boolean> = _isWavPlaying.asStateFlow()

    private val _loadedFileName = MutableStateFlow<String?>(null)
    val loadedFileName: StateFlow<String?> = _loadedFileName.asStateFlow()

    private val _wavRepeat = MutableStateFlow(false)
    val wavRepeat: StateFlow<Boolean> = _wavRepeat.asStateFlow()

    private val _wavProcessEffects = MutableStateFlow(false)
    val wavProcessEffects: StateFlow<Boolean> = _wavProcessEffects.asStateFlow()

    // Recording state
    private val _isRecording = MutableStateFlow(false)
    val isRecording: StateFlow<Boolean> = _isRecording.asStateFlow()

    private val _recordingDurationSec = MutableStateFlow(0.0)
    val recordingDurationSec: StateFlow<Double> = _recordingDurationSec.asStateFlow()

    init {
        val ctx = getApplication<Application>()
        val inputId = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getInputDeviceId(ctx)
        val outputId = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getOutputDeviceId(ctx)
        val bursts = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getBufferBursts(ctx)
        val cushionMs = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getInputCushionMs(ctx)
        startEngine(inputDeviceId = inputId, outputDeviceId = outputId, bufferBursts = bursts, inputCushionMs = cushionMs)
        updateRackState()

        // Refresh rack state periodically to catch external changes
        viewModelScope.launch {
            delay(1000)
            updateRackState()
        }

        // Poll meters when engine is running
        viewModelScope.launch {
            var cpuSum = 0f
            var latencySum = 0.0
            var sampleCount = 0
            while (true) {
                if (_isEngineRunning.value) {
                    try {
                        _inputLevel.value = AudioEngine.getInputLevel()
                        _outputLevel.value = AudioEngine.getOutputLevel()
                        _inputClipping.value = AudioEngine.isInputClipping()
                        _outputClipping.value = AudioEngine.isOutputClipping()
                        cpuSum += AudioEngine.getCpuLoad()
                        latencySum += AudioEngine.getLatencyMs()
                        sampleCount++
                        if (sampleCount >= 20) { // ~1 second (20 × 50ms)
                            _cpuLoad.value = cpuSum / sampleCount
                            _latencyMs.value = latencySum / sampleCount
                            _xRunCount.value = AudioEngine.getXRunCount()
                            cpuSum = 0f
                            latencySum = 0.0
                            sampleCount = 0
                        }
                        // Poll recording duration
                        if (_isRecording.value) {
                            _recordingDurationSec.value = RecordingManager.getRecordingDurationSec()
                        }
                    } catch (_: Exception) { }
                } else {
                    cpuSum = 0f
                    latencySum = 0.0
                    sampleCount = 0
                }
                delay(50)
            }
        }
    }

    fun startEngine(inputDeviceId: Int = 0, outputDeviceId: Int = 0, bufferBursts: Int = 4, inputCushionMs: Int = 0) {
        android.util.Log.i("AudioLifecycle", "RackViewModel.startEngine(input=$inputDeviceId, output=$outputDeviceId, bursts=$bufferBursts, cushionMs=$inputCushionMs) (thread=${Thread.currentThread().name})")
        viewModelScope.launch {
            try {
                val started = AudioEngine.start(
                    inputDeviceId = inputDeviceId,
                    outputDeviceId = outputDeviceId,
                    bufferBursts = bufferBursts,
                    inputCushionMs = inputCushionMs
                )
                android.util.Log.i("AudioLifecycle", "RackViewModel.startEngine() result=$started")
                _isEngineRunning.value = started
                if (started) {
                    _errorMessage.value = null
                    // 恢复链首输入增益 + 噪声门 + 输出增益设置（对齐用户在设置页的保存值）
                    val appCtx = getApplication<Application>().applicationContext
                    com.varcain.guitarrackcraft.engine.AudioEngine.apply {
                        setPreGainDb(com.varcain.guitarrackcraft.engine.AudioSettingsManager.getPreGainDb(appCtx))
                        setGateThresholdDb(com.varcain.guitarrackcraft.engine.AudioSettingsManager.getGateThresholdDb(appCtx))
                        setGateHysteresisDb(com.varcain.guitarrackcraft.engine.AudioSettingsManager.getGateHysteresisDb(appCtx))
                        setGateFloorDb(com.varcain.guitarrackcraft.engine.AudioSettingsManager.getGateFloorDb(appCtx))
                        setGateAttackMs(com.varcain.guitarrackcraft.engine.AudioSettingsManager.getGateAttackMs(appCtx))
                        setGateHoldMs(com.varcain.guitarrackcraft.engine.AudioSettingsManager.getGateHoldMs(appCtx))
                        setGateReleaseMs(com.varcain.guitarrackcraft.engine.AudioSettingsManager.getGateReleaseMs(appCtx))
                        setOutputGainDb(com.varcain.guitarrackcraft.engine.AudioSettingsManager.getOutputGainDb(appCtx))
                    }
                    // 前台服务保活：切后台/锁屏后音频回调不被冻结
                    AudioForegroundService.start(getApplication())
                    // 禁用系统在输出会话上插入的音效（Dolby/MiSound 等）——
                    // 它们会压缩/限幅吉他信号导致破音。OEM 策略可能在流启动
                    // 后一小会儿才挂效果，所以延迟重试几次。
                    disableSystemAudioEffects()
                } else {
                    // 启动失败也要给出明确反馈，避免“点了没反应”
                    _errorMessage.value = ctx().getString(
                        R.string.rack_err_start_engine,
                        ctx().getString(R.string.rack_err_engine_audio_fail)
                    )
                }
            } catch (e: Exception) {
                _errorMessage.value = ctx().getString(R.string.rack_err_start_engine, e.message)
            }
        }
    }

    private var restartJob: Job? = null

    /**
     * 禁用系统音效（Dolby/MiSound 等输出后处理）。OEM 音频策略可能在流
     * 启动后延迟挂效果，因此立即尝试一次，之后在 1s / 3s 再各试一次。
     */
    private fun disableSystemAudioEffects() {
        viewModelScope.launch {
            for (delayMs in longArrayOf(0, 1000, 3000)) {
                if (delayMs > 0) delay(delayMs)
                if (!_isEngineRunning.value) return@launch
                val sessionId = AudioEngine.getOutputSessionId()
                if (sessionId > 0) {
                    val n = com.varcain.guitarrackcraft.engine.AudioEffectDisabler
                        .disableSystemEffects(sessionId)
                    if (n > 0) return@launch
                }
            }
        }
    }

    fun restartEngine(context: Context) {
        restartJob?.cancel()
        restartJob = viewModelScope.launch {
            val inputId = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getInputDeviceId(context)
            val outputId = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getOutputDeviceId(context)
            val bursts = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getBufferBursts(context)
            val cushionMs = com.varcain.guitarrackcraft.engine.AudioSettingsManager.getInputCushionMs(context)
            android.util.Log.i("AudioLifecycle", "RackViewModel.restartEngine(input=$inputId, output=$outputId, bursts=$bursts, cushionMs=$cushionMs)")
            stopEngine()
            delay(100)
            startEngine(inputDeviceId = inputId, outputDeviceId = outputId, bufferBursts = bursts, inputCushionMs = cushionMs)
        }
    }

    fun resetClipping() {
        AudioEngine.resetClipping()
        _inputClipping.value = false
        _outputClipping.value = false
    }

    fun stopEngine() {
        android.util.Log.i("AudioLifecycle", "RackViewModel.stopEngine() -> native (thread=${Thread.currentThread().name})")
        stopRecording()
        AudioEngine.stop()
        AudioForegroundService.stop(getApplication())
        _isEngineRunning.value = false
    }

    fun toggleRecording(context: Context) {
        if (_isRecording.value) {
            stopRecording()
        } else {
            if (!_isEngineRunning.value) {
                _errorMessage.value = ctx().getString(R.string.rack_err_record_need_engine)
                return
            }
            val started = RecordingManager.startRecording(context)
            _isRecording.value = started
            if (!started) {
                _errorMessage.value = ctx().getString(R.string.rack_err_start_recording)
            }
        }
    }

    fun stopRecording() {
        if (_isRecording.value) {
            RecordingManager.stopRecording()
            _isRecording.value = false
            _recordingDurationSec.value = 0.0
        }
    }

    fun removePlugin(position: Int) {
        // Off the main thread — VST removal triggers WineVstPlugin::deactivate
        // which waits up to 3s for the wine subprocess to exit + ~2s for the
        // X11 server's serverLoop to drain. On the main thread this trips
        // Android's input-dispatch ANR (5s) and the OS force-closes the app.
        viewModelScope.launch(Dispatchers.IO) {
            try {
                if (RackManager.removePlugin(position)) {
                    updateRackState()
                } else {
                    _errorMessage.value = ctx().getString(R.string.rack_err_remove_plugin_pos, position)
                }
            } catch (e: Exception) {
                _errorMessage.value = ctx().getString(R.string.rack_err_remove_plugin, e.message)
            }
        }
    }

    fun reorderPlugins(fromPos: Int, toPos: Int) {
        viewModelScope.launch {
            try {
                if (RackManager.reorder(fromPos, toPos)) {
                    updateRackState()
                } else {
                    _errorMessage.value = ctx().getString(R.string.rack_err_reorder_plugins)
                }
            } catch (e: Exception) {
                _errorMessage.value = ctx().getString(R.string.rack_err_reorder_plugins_msg, e.message)
            }
        }
    }

    fun refreshRack(forceNewInstanceIds: Boolean = false) {
        updateRackState(forceNewInstanceIds)
    }

    fun loadWav(path: String, fileName: String? = null) {
        viewModelScope.launch {
            try {
                if (!AudioEngine.isRunning()) {
                    _errorMessage.value = ctx().getString(R.string.rack_engine_needed_for_wav)
                    return@launch
                }
                val success = withContext(Dispatchers.IO) {
                    WavPlayer.load(path)
                }
                if (success) {
                    _wavLoaded.value = true
                    _wavDurationSec.value = WavPlayer.getDurationSec()
                    _wavPositionSec.value = 0.0
                    _isWavPlaying.value = false
                    _loadedFileName.value = fileName ?: path.substringAfterLast('/')
                    _errorMessage.value = null
                } else {
                    _errorMessage.value = ctx().getString(R.string.rack_err_load_wav)
                }
            } catch (e: Exception) {
                _errorMessage.value = ctx().getString(R.string.rack_err_load_wav_msg, e.message)
            }
        }
    }

    fun wavPlay() {
        try {
            WavPlayer.play()
            _isWavPlaying.value = WavPlayer.isPlaying()
        } catch (_: Exception) { }
    }

    fun wavPause() {
        try {
            WavPlayer.pause()
            _isWavPlaying.value = false
        } catch (_: Exception) { }
    }

    fun wavSeek(positionSec: Double) {
        try {
            WavPlayer.seek(positionSec)
            _wavPositionSec.value = positionSec
        } catch (_: Exception) { }
    }

    fun updateWavPosition() {
        try {
            _wavPositionSec.value = WavPlayer.getPositionSec()
            _isWavPlaying.value = WavPlayer.isPlaying()
        } catch (_: Exception) { }
    }

    fun unloadWav() {
        try {
            WavPlayer.unload()
            _wavLoaded.value = false
            _wavDurationSec.value = 0.0
            _wavPositionSec.value = 0.0
            _isWavPlaying.value = false
            _loadedFileName.value = null
        } catch (_: Exception) { }
    }

    fun wavRestart() {
        wavSeek(0.0)
        wavPlay()
    }

    fun wavToggleRepeat() {
        _wavRepeat.value = !_wavRepeat.value
    }

    fun wavToggleProcessEffects() {
        setWavProcessEffects(!_wavProcessEffects.value)
    }

    fun setWavProcessEffects(enabled: Boolean) {
        _wavProcessEffects.value = enabled
        NativeEngine.getInstance().setWavBypassChain(!enabled)
    }

    fun setPluginFilePath(pluginIndex: Int, propertyUri: String, filePath: String) {
        viewModelScope.launch {
            try {
                com.varcain.guitarrackcraft.engine.NativeEngine.getInstance()
                    .setPluginFilePath(pluginIndex, propertyUri, filePath)
                // Neuralrack runs NAM inference on the audio callback thread by default.
                // Heavy models can overrun the callback budget on mid-range SoCs (e.g.
                // Redmi K80) and cause periodic underruns -> intermittent crackling.
                // Its "buffered" port (index 20) moves inference to a background thread,
                // eliminating callback overruns at the cost of +1 block (~4 ms) latency.
                if (propertyUri == "urn:brummer:neuralrack#Neural_Model") {
                    RackManager.setParameter(pluginIndex, 20, 1f)
                    // Enable input normalization for Slot A: NAM models expect a
                    // hot input (near full-scale) but guitar pickups deliver
                    // only ~-40..-20 dBFS. Without normalization the model runs
                    // under-driven (thin, dull tone) while its internal gain
                    // still amplifies the noise floor. Normalizing lifts the
                    // input to the model's working level, improving tone and
                    // signal-to-noise ratio (same approach as NAM droid).
                    RackManager.setParameter(pluginIndex, 12, 1f)
                }
            } catch (e: Exception) {
                _errorMessage.value = ctx().getString(R.string.rack_err_set_file_path, e.message)
            }
        }
    }

    fun setParameter(pluginIndex: Int, portIndex: Int, value: Float) {
        viewModelScope.launch {
            try {
                RackManager.setParameter(pluginIndex, portIndex, value)
            } catch (e: Exception) {
                _errorMessage.value = ctx().getString(R.string.rack_err_set_parameter, e.message)
            }
        }
    }

    fun getParameter(pluginIndex: Int, portIndex: Int): Float {
        return try {
            RackManager.getParameter(pluginIndex, portIndex)
        } catch (e: Exception) {
            0.0f
        }
    }

    /**
     * Returns the UI type to use for this plugin: last user choice if stored and still available,
     * otherwise the default (MODGUI > Native > Sliders).
     */
    fun getPreferredUiTypeForPlugin(pluginInfo: PluginInfo): UiType {
        val ctx = getApplication<Application>()
        val stored = PluginUiPreferenceManager.getStoredUiType(ctx, pluginInfo.fullId)
        return if (stored != null && pluginInfo.guiTypes.contains(stored)) stored
        else pluginInfo.preferredUiType
    }

    /** Persists the user's UI choice for this plugin so it is used when the plugin is added again. */
    fun setPreferredUiTypeForPlugin(pluginFullId: String, uiType: UiType) {
        PluginUiPreferenceManager.setStoredUiType(getApplication<Application>(), pluginFullId, uiType)
    }

    private suspend fun updateRackStateNow(forceNewInstanceIds: Boolean = false) {
        try {
            val plugins = RackManager.getRackPlugins()
            val oldList = _rackPlugins.value
            // Preserve instanceIds for plugins that stayed in the rack,
            // unless forceNewInstanceIds is set (e.g. after preset load which
            // removes and re-adds all plugins — the native UIs are detached
            // and Compose must tear down and recreate views).
            val availableOld = if (forceNewInstanceIds) emptyMap()
                else oldList.groupBy { it.pluginId }.mapValues { it.value.toMutableList() }
            _rackPlugins.value = plugins.mapIndexed { index, pluginInfo ->
                val fullId = pluginInfo.fullId
                val existing = availableOld[fullId]?.removeFirstOrNull()
                RackPlugin(
                    index = index,
                    name = pluginInfo.name.ifEmpty { pluginInfo.id },
                    pluginId = fullId,
                    instanceId = existing?.instanceId ?: RackPlugin.nextInstanceId()
                )
            }
            android.util.Log.i("AudioLifecycle", "updateRackState: ok size=${plugins.size} forceNewInstanceIds=$forceNewInstanceIds")
        } catch (e: Exception) {
            _errorMessage.value = ctx().getString(R.string.rack_err_get_rack_plugins, e.message)
            android.util.Log.e("AudioLifecycle", "updateRackState: failed (keeping previous list to avoid tearing down X11 UIs): ${e.message}", e)
        }
    }

    private fun updateRackState(forceNewInstanceIds: Boolean = false) {
        viewModelScope.launch {
            updateRackStateNow(forceNewInstanceIds)
        }
    }

    private suspend fun <T> withBlockingOperation(label: String, block: suspend () -> T): T {
        _blockingOperation.value = label
        return try {
            block()
        } finally {
            _blockingOperation.value = null
        }
    }

    private fun ensureRecentManager(ctx: Context): RecentPresetsManager {
        return recentPresetsManager ?: RecentPresetsManager(ctx).also { recentPresetsManager = it }
    }

    fun refreshPresets(ctx: Context) {
        _presetList.value = presetManager.listPresets(ctx)
        _recentPresets.value = ensureRecentManager(ctx).getRecents()
    }

    fun savePreset(ctx: Context, name: String) {
        viewModelScope.launch {
            val ok = try {
                withBlockingOperation(ctx().getString(R.string.rack_op_saving_preset)) {
                    withContext(Dispatchers.IO) {
                        presetManager.savePreset(ctx, name)
                    }
                }
            } catch (e: Exception) {
                android.util.Log.e("AudioLifecycle", "savePreset failed: ${e.message}", e)
                false
            }
            if (ok) {
                ensureRecentManager(ctx).addRecent(name)
                refreshPresets(ctx)
                _presetMessage.value = ctx().getString(R.string.rack_preset_saved, name)
            } else {
                _presetMessage.value = ctx().getString(R.string.rack_preset_save_failed)
            }
        }
    }

    fun loadPreset(ctx: Context, name: String) {
        viewModelScope.launch {
            val engine = NativeEngine.getInstance()
            val ok = try {
                withBlockingOperation(ctx().getString(R.string.rack_op_loading_preset)) {
                    val loaded = withContext(Dispatchers.IO) {
                        engine.setChainBypass(true)
                        try {
                            presetManager.loadPreset(ctx, name)
                        } finally {
                            engine.setChainBypass(false)
                        }
                    }
                    if (loaded) {
                        ensureRecentManager(ctx).addRecent(name)
                        refreshPresets(ctx)
                        updateRackStateNow(forceNewInstanceIds = true)
                    }
                    loaded
                }
            } catch (e: Exception) {
                android.util.Log.e("AudioLifecycle", "loadPreset failed: ${e.message}", e)
                false
            }
            if (ok) {
                _presetMessage.value = ctx().getString(R.string.rack_preset_loaded, name)
            } else {
                _presetMessage.value = ctx().getString(R.string.rack_preset_load_failed_mismatch)
            }
        }
    }

    fun loadRecordingPreset(json: String) {
        viewModelScope.launch {
            val engine = NativeEngine.getInstance()
            val ok = try {
                withBlockingOperation(ctx().getString(R.string.rack_op_loading_preset)) {
                    val loaded = withContext(Dispatchers.IO) {
                        engine.setChainBypass(true)
                        try {
                            presetManager.loadPresetFromJson(json)
                        } finally {
                            engine.setChainBypass(false)
                        }
                    }
                    if (loaded) {
                        updateRackStateNow(forceNewInstanceIds = true)
                    }
                    loaded
                }
            } catch (e: Exception) {
                android.util.Log.e("AudioLifecycle", "loadRecordingPreset failed: ${e.message}", e)
                false
            }
            if (ok) {
                _presetMessage.value = ctx().getString(R.string.rack_recording_preset_loaded)
            } else {
                _presetMessage.value = ctx().getString(R.string.rack_recording_preset_failed)
            }
        }
    }

    fun deletePreset(ctx: Context, name: String) {
        viewModelScope.launch {
            presetManager.deletePreset(ctx, name)
            ensureRecentManager(ctx).removeRecent(name)
            refreshPresets(ctx)
            _presetMessage.value = ctx().getString(R.string.rack_preset_deleted, name)
        }
    }

    fun getPresetJson(ctx: Context, name: String): String? {
        return presetManager.getPresetJson(ctx, name)
    }

    fun clearRecentPresets(ctx: Context) {
        ensureRecentManager(ctx).clearRecents()
        _recentPresets.value = emptyList()
    }

    fun clearError() {
        _errorMessage.value = null
    }

    fun clearPresetMessage() {
        _presetMessage.value = null
    }

    override fun onCleared() {
        super.onCleared()
    }
}
