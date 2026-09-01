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

data class AudioDeviceOption(
    val id: Int,
    val name: String,
    val type: Int
)

object AudioSettingsManager {
    private const val PREFS_NAME = "audio_settings"
    private const val KEY_INPUT_DEVICE_ID = "inputDeviceId"
    private const val KEY_OUTPUT_DEVICE_ID = "outputDeviceId"
    private const val KEY_BUFFER_SIZE = "bufferSize"

    val BUFFER_SIZE_OPTIONS = listOf(
        0 to "Auto",
        16 to "16",
        32 to "32",
        64 to "64",
        128 to "128",
        256 to "256",
        512 to "512",
        1024 to "1024"
    )

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

    fun getBufferSize(context: Context): Int =
        prefs(context).getInt(KEY_BUFFER_SIZE, 0)

    fun setBufferSize(context: Context, size: Int) {
        prefs(context).edit().putInt(KEY_BUFFER_SIZE, size).apply()
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
