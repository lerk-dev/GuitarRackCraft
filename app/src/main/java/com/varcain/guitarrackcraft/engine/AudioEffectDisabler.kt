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

import android.media.audiofx.AudioEffect
import android.util.Log
import java.util.UUID

/**
 * Disables system post-processing effects that OEM audio policies attach to
 * music output sessions. On several devices (Xiaomi with Dolby/MiSound being
 * the worst offenders) an EQ/compression/limiting chain is inserted on the
 * app's output track. For music playback this can be tolerable, but for a
 * live guitar signal it causes pumping, distortion and clipping, and the
 * effect thread occasionally blocks for tens of milliseconds.
 *
 * The UUID-based AudioEffect constructor needed to attach to these existing
 * OEM effects is not part of the public SDK, so it is invoked via reflection
 * (it sits on the unsupported-but-accessible greylist).
 */
object AudioEffectDisabler {

    private const val TAG = "AudioEffectDisabler"

    /**
     * Effect implementations known to be inserted on output sessions and to
     * audibly degrade a live instrument signal, keyed by implementation UUID
     * (for these OEM effects the type UUID equals the implementation UUID).
     */
    private val KNOWN_DEGRADING_EFFECT_UUIDS = setOf(
        // Dolby "Music Listener" (Dolby Laboratories) — seen on Xiaomi devices
        "40f66c8b-5aa5-4345-8919-53ec431aaa98",
        // Qualcomm "Volume listener for Music" — mostly passive, disable anyway
        "08b8b058-0590-11e5-ac71-0025b32654a0",
        // MiSound Audio Effect (Xiaomi post-processing)
        "47f92770-5c8f-11e9-8f9e-2a86e4085a59"
    )

    // AudioEffect(UUID type, UUID uuid, int priority, int audioSession)
    private val ctor by lazy {
        try {
            AudioEffect::class.java.getDeclaredConstructor(
                UUID::class.java, UUID::class.java,
                Int::class.javaPrimitiveType, Int::class.javaPrimitiveType
            ).also { it.isAccessible = true }
        } catch (e: Exception) {
            Log.w(TAG, "UUID AudioEffect constructor not available", e)
            null
        }
    }

    /**
     * Disable all known degrading effects on the given audio session.
     *
     * @param sessionId audio session id of the output stream (0 = none)
     * @return number of effects that were successfully disabled
     */
    fun disableSystemEffects(sessionId: Int): Int {
        if (sessionId <= 0) return 0
        val constructor = ctor ?: return 0
        var disabled = 0
        val descriptors = try {
            AudioEffect.queryEffects()
        } catch (e: Exception) {
            Log.w(TAG, "queryEffects failed", e)
            return 0
        } ?: return 0

        for (desc in descriptors) {
            val uuidStr = desc.uuid?.toString() ?: continue
            if (uuidStr !in KNOWN_DEGRADING_EFFECT_UUIDS) continue
            try {
                val effect = constructor.newInstance(desc.uuid, desc.uuid, 0, sessionId) as AudioEffect
                try {
                    if (effect.enabled) {
                        effect.enabled = false
                        Log.i(TAG, "Disabled ${desc.name ?: uuidStr} on session $sessionId")
                        disabled++
                    }
                } finally {
                    effect.release()
                }
            } catch (e: Exception) {
                // Effect not present on this session or control refused — fine.
                Log.d(TAG, "Could not disable ${desc.name ?: uuidStr}: ${e.message}")
            }
        }
        if (disabled == 0) {
            Log.i(TAG, "No degrading system effects found on session $sessionId")
        }
        return disabled
    }
}
