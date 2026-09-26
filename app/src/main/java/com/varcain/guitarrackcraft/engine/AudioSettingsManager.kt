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

package com.varcain.guitarrackcraft.engine

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import com.varcain.guitarrackcraft.R
import kotlin.math.roundToInt

data class AudioDeviceOption(
    val id: Int,
    val name: String,
    val type: Int
)

object AudioSettingsManager {
    private const val PREFS_NAME = "audio_settings"
    private const val KEY_INPUT_DEVICE_ID = "inputDeviceId"
    private const val KEY_OUTPUT_DEVICE_ID = "outputDeviceId"
    private const val KEY_BUFFER_BURSTS = "bufferBursts"
    /** Legacy fixed-frame-size key, kept only for the one-time migration. */
    private const val KEY_BUFFER_SIZE_LEGACY = "bufferSize"
    private const val KEY_INPUT_CUSHION_MS = "inputCushionMs"
    private const val KEY_PRE_GAIN_DB = "preGainDb"
    private const val KEY_GATE_THRESHOLD_DB = "gateThresholdDb"
    private const val KEY_GATE_HYSTERESIS_DB = "gateHysteresisDb"
    private const val KEY_GATE_FLOOR_DB = "gateFloorDb"
    private const val KEY_GATE_ATTACK_MS = "gateAttackMs"
    private const val KEY_GATE_HOLD_MS = "gateHoldMs"
    private const val KEY_GATE_RELEASE_MS = "gateReleaseMs"
    private const val KEY_OUTPUT_GAIN_DB = "outputGainDb"

    /** Noise gate disabled sentinel (threshold at/below -96 dB). */
    const val GATE_OFF = -120f
    /** Default gate threshold: on by default so guitar-off noise is muted. */
    const val GATE_DEFAULT = -60f
    val GATE_RANGE = -80f..-20f

    /** Gate closes to this depth instead of full silence, so tails release naturally. */
    const val GATE_FLOOR_DEFAULT = -80f
    val GATE_FLOOR_RANGE = -100f..-20f

    const val GATE_HYSTERESIS_DEFAULT = 3f
    val GATE_HYSTERESIS_RANGE = 0f..12f

    const val GATE_ATTACK_DEFAULT = 5f
    val GATE_ATTACK_RANGE = 0.5f..50f

    const val GATE_HOLD_DEFAULT = 50f
    val GATE_HOLD_RANGE = 0f..500f

    const val GATE_RELEASE_DEFAULT = 100f
    val GATE_RELEASE_RANGE = 10f..1000f

    /** Default buffer tier: N x hardware burst (NAM Sandwich's Balanced). */
    const val BUFFER_BURSTS_DEFAULT = 4
    /** Buffer tiers as multiples of the hardware burst. */
    val BUFFER_BURSTS_TIERS = listOf(2, 4, 6, 8)
    /** The 1x tier is only offered when the hardware burst is at least this long. */
    const val BUFFER_BURSTS_TIER1_MIN_BURST_MS = 5.0f

    /** Input cushion choices in milliseconds. */
    val CUSHION_CHOICES_MS = listOf(0, 5, 10, 20, 30, 40)
    const val CUSHION_DEFAULT_MS = 0

    /**
     * Buffer tiers available on this device. A 1x burst buffer is only safe when
     * the burst itself is long enough (>= 5 ms), otherwise every callback xruns.
     */
    fun bufferBurstTiers(burstFrames: Int, sampleRate: Float): List<Int> =
        if (burstFrames > 0 && sampleRate > 0f &&
            burstFrames / sampleRate * 1000f >= BUFFER_BURSTS_TIER1_MIN_BURST_MS
        ) {
            listOf(1) + BUFFER_BURSTS_TIERS
        } else {
            BUFFER_BURSTS_TIERS
        }

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun getInputDeviceId(context: Context): Int =
        prefs(context).getInt(KEY_INPUT_DEVICE_ID, 0)

    fun setInputDeviceId(context: Context, id: Int) {
        prefs(context).edit().putInt(KEY_INPUT_DEVICE_ID, id).apply()
    }

    fun getOutputDeviceId(context: Context): Int =
        prefs(context).getInt(KEY_OUTPUT_DEVICE_ID, 0)

    fun setOutputDeviceId(context: Context, id: Int) {
        prefs(context).edit().putInt(KEY_OUTPUT_DEVICE_ID, id).apply()
    }

    /**
     * Buffer tier in multiples of the hardware burst. On first read after an
     * upgrade, the legacy fixed frame size is migrated once and then dropped:
     * 0 (Auto) -> default tier, otherwise frames / 128 snapped to an even tier.
     */
    fun getBufferBursts(context: Context): Int {
        val p = prefs(context)
        if (p.contains(KEY_BUFFER_BURSTS)) {
            return p.getInt(KEY_BUFFER_BURSTS, BUFFER_BURSTS_DEFAULT)
        }
        val legacy = p.getInt(KEY_BUFFER_SIZE_LEGACY, 0)
        val migrated = if (legacy <= 0) {
            BUFFER_BURSTS_DEFAULT
        } else {
            (((legacy / 128f).roundToInt() / 2) * 2).coerceIn(2, 8)
        }
        p.edit().putInt(KEY_BUFFER_BURSTS, migrated).remove(KEY_BUFFER_SIZE_LEGACY).apply()
        return migrated
    }

    fun setBufferBursts(context: Context, bursts: Int) {
        prefs(context).edit().putInt(KEY_BUFFER_BURSTS, bursts).apply()
    }

    fun getInputCushionMs(context: Context): Int =
        prefs(context).getInt(KEY_INPUT_CUSHION_MS, CUSHION_DEFAULT_MS)

    fun setInputCushionMs(context: Context, ms: Int) {
        prefs(context).edit().putInt(KEY_INPUT_CUSHION_MS, ms).apply()
    }

    fun getPreGainDb(context: Context): Float =
        prefs(context).getFloat(KEY_PRE_GAIN_DB, 0f)

    fun setPreGainDb(context: Context, db: Float) {
        prefs(context).edit().putFloat(KEY_PRE_GAIN_DB, db).apply()
    }

    fun getGateThresholdDb(context: Context): Float =
        prefs(context).getFloat(KEY_GATE_THRESHOLD_DB, GATE_DEFAULT)

    fun setGateThresholdDb(context: Context, db: Float) {
        prefs(context).edit().putFloat(KEY_GATE_THRESHOLD_DB, db).apply()
    }

    fun getGateHysteresisDb(context: Context): Float =
        prefs(context).getFloat(KEY_GATE_HYSTERESIS_DB, GATE_HYSTERESIS_DEFAULT)

    fun setGateHysteresisDb(context: Context, db: Float) {
        prefs(context).edit().putFloat(KEY_GATE_HYSTERESIS_DB, db).apply()
    }

    fun getGateFloorDb(context: Context): Float =
        prefs(context).getFloat(KEY_GATE_FLOOR_DB, GATE_FLOOR_DEFAULT)

    fun setGateFloorDb(context: Context, db: Float) {
        prefs(context).edit().putFloat(KEY_GATE_FLOOR_DB, db).apply()
    }

    fun getGateAttackMs(context: Context): Float =
        prefs(context).getFloat(KEY_GATE_ATTACK_MS, GATE_ATTACK_DEFAULT)

    fun setGateAttackMs(context: Context, ms: Float) {
        prefs(context).edit().putFloat(KEY_GATE_ATTACK_MS, ms).apply()
    }

    fun getGateHoldMs(context: Context): Float =
        prefs(context).getFloat(KEY_GATE_HOLD_MS, GATE_HOLD_DEFAULT)

    fun setGateHoldMs(context: Context, ms: Float) {
        prefs(context).edit().putFloat(KEY_GATE_HOLD_MS, ms).apply()
    }

    fun getGateReleaseMs(context: Context): Float =
        prefs(context).getFloat(KEY_GATE_RELEASE_MS, GATE_RELEASE_DEFAULT)

    fun setGateReleaseMs(context: Context, ms: Float) {
        prefs(context).edit().putFloat(KEY_GATE_RELEASE_MS, ms).apply()
    }

    fun getOutputGainDb(context: Context): Float =
        prefs(context).getFloat(KEY_OUTPUT_GAIN_DB, 0f)

    fun setOutputGainDb(context: Context, db: Float) {
        prefs(context).edit().putFloat(KEY_OUTPUT_GAIN_DB, db).apply()
    }

    fun getInputDevices(context: Context): List<AudioDeviceOption> {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val devices = am.getDevices(AudioManager.GET_DEVICES_INPUTS)
        return buildList {
            add(AudioDeviceOption(0, context.getString(R.string.settings_default_device), 0))
            devices.forEach { dev ->
                add(AudioDeviceOption(dev.id, deviceDisplayName(context, dev), dev.type))
            }
        }
    }

    fun getOutputDevices(context: Context): List<AudioDeviceOption> {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val devices = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        return buildList {
            add(AudioDeviceOption(0, context.getString(R.string.settings_default_device), 0))
            devices.forEach { dev ->
                add(AudioDeviceOption(dev.id, deviceDisplayName(context, dev), dev.type))
            }
        }
    }

    /**
     * Build a friendly display name for an audio device.
     * Built-in devices (mic/speaker/earpiece) often report a serial number or
     * board codename as productName — show the phone model (Build.MODEL) instead.
     * External devices (USB/Bluetooth) keep their real product name.
     */
    private fun deviceDisplayName(context: Context, dev: AudioDeviceInfo): String {
        val typeName = deviceTypeName(context, dev.type)
        val isBuiltIn = when (dev.type) {
            AudioDeviceInfo.TYPE_BUILTIN_MIC,
            AudioDeviceInfo.TYPE_BUILTIN_SPEAKER,
            AudioDeviceInfo.TYPE_BUILTIN_EARPIECE,
            AudioDeviceInfo.TYPE_TELEPHONY -> true
            else -> false
        }
        val name = if (isBuiltIn) {
            android.os.Build.MODEL?.takeIf { it.isNotBlank() }
                ?: android.os.Build.DEVICE?.takeIf { it.isNotBlank() }
                ?: "Android"
        } else {
            dev.productName?.toString()?.ifEmpty { null } ?: typeName
        }
        return "$name ($typeName)"
    }

    private fun deviceTypeName(context: Context, type: Int): String = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> context.getString(R.string.device_type_builtin_mic)
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> context.getString(R.string.device_type_builtin_speaker)
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> context.getString(R.string.device_type_builtin_earpiece)
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> context.getString(R.string.device_type_wired_headset)
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> context.getString(R.string.device_type_wired_headphones)
        AudioDeviceInfo.TYPE_USB_DEVICE -> context.getString(R.string.device_type_usb_device)
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> context.getString(R.string.device_type_usb_accessory)
        AudioDeviceInfo.TYPE_USB_HEADSET -> context.getString(R.string.device_type_usb_headset)
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> context.getString(R.string.device_type_bluetooth_sco)
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> context.getString(R.string.device_type_bluetooth_a2dp)
        AudioDeviceInfo.TYPE_TELEPHONY -> context.getString(R.string.device_type_telephony)
        AudioDeviceInfo.TYPE_AUX_LINE -> context.getString(R.string.device_type_aux_line)
        AudioDeviceInfo.TYPE_HDMI -> context.getString(R.string.device_type_hdmi)
        AudioDeviceInfo.TYPE_HDMI_ARC -> context.getString(R.string.device_type_hdmi_arc)
        else -> context.getString(R.string.device_type_audio_device)
    }
}
