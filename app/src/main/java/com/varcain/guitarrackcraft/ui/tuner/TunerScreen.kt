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

package com.varcain.guitarrackcraft.ui.tuner

import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.varcain.guitarrackcraft.R
import com.varcain.guitarrackcraft.engine.AudioEngine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlin.math.abs
import kotlin.math.log2
import kotlin.math.roundToInt

private val NOTE_NAMES = arrayOf("C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B")

/** Standard guitar tuning (low to high), used for the "nearest string" hint. */
private val GUITAR_STRINGS = listOf(
    "E2" to 82.41f,
    "A2" to 110.00f,
    "D3" to 146.83f,
    "G3" to 196.00f,
    "B3" to 246.94f,
    "E4" to 329.63f
)

/** In-tune tolerance in cents. */
private const val IN_TUNE_CENTS = 5f

private data class PitchReading(
    val frequency: Float,   // 0 = no pitch detected
    val noteName: String,   // e.g. "E2"
    val cents: Float,       // -50..+50
    val inTune: Boolean
)

private fun analyzeFrequency(freq: Float): PitchReading {
    if (freq <= 0f) return PitchReading(0f, "", 0f, false)
    val midi = 69.0 + 12.0 * log2(freq / 440.0)
    val nearest = midi.roundToInt()
    val cents = ((midi - nearest) * 100).toFloat()
    val noteIdx = ((nearest % 12) + 12) % 12
    val octave = (nearest - noteIdx) / 12 - 1
    return PitchReading(freq, "${NOTE_NAMES[noteIdx]}$octave", cents, abs(cents) <= IN_TUNE_CENTS)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TunerScreen(
    onNavigateBack: () -> Unit
) {
    BackHandler { onNavigateBack() }

    // Enable pitch detection while the screen is visible
    DisposableEffect(Unit) {
        AudioEngine.tunerSetEnabled(true)
        onDispose { AudioEngine.tunerSetEnabled(false) }
    }

    // Poll the detector (analysis runs on its own native worker; we just read state)
    var reading by remember { mutableStateOf(PitchReading(0f, "", 0f, false)) }
    LaunchedEffect(Unit) {
        while (true) {
            val freq = withContext(Dispatchers.IO) { AudioEngine.tunerGetFrequency() }
            reading = analyzeFrequency(freq)
            delay(60)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.tuner_title)) },
                navigationIcon = {
                    IconButton(onClick = { onNavigateBack() }) {
                        Icon(Icons.Default.ArrowBack, contentDescription = stringResource(R.string.common_back))
                    }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            // Note name + frequency
            Text(
                text = reading.noteName.ifEmpty { stringResource(R.string.tuner_play_a_note) },
                style = MaterialTheme.typography.displayLarge,
                fontWeight = FontWeight.Bold,
                color = when {
                    reading.frequency <= 0f -> MaterialTheme.colorScheme.onSurfaceVariant
                    reading.inTune -> Color(0xFF4CAF50)
                    else -> MaterialTheme.colorScheme.onSurface
                }
            )
            if (reading.frequency > 0f) {
                Text(
                    text = "%.1f Hz".format(reading.frequency),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            Spacer(Modifier.height(32.dp))

            // Cents meter
            CentsMeter(
                cents = reading.cents,
                active = reading.frequency > 0f,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(160.dp)
            )
            Row(
                modifier = Modifier.fillMaxWidth(0.85f),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("♭", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text("♪", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text("♯", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }

            Spacer(Modifier.height(16.dp))

            // Cents readout + in-tune indicator
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                Text(
                    text = if (reading.frequency > 0f) {
                        (if (reading.cents >= 0) "+" else "") + "%.0f".format(reading.cents) + " ¢"
                    } else "-- ¢",
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.Bold,
                    color = if (reading.inTune && reading.frequency > 0f) Color(0xFF4CAF50)
                    else MaterialTheme.colorScheme.onSurfaceVariant
                )
                InTuneDot(inTune = reading.inTune && reading.frequency > 0f)
            }

            Spacer(Modifier.height(24.dp))

            // Nearest standard-tuning string hint
            if (reading.frequency > 0f) {
                val nearest = GUITAR_STRINGS.minByOrNull { abs(log2(it.second / reading.frequency)) }
                if (nearest != null && abs(log2(nearest.second / reading.frequency)) < 0.04) { // within ~70 cents
                    Text(
                        text = stringResource(R.string.tuner_nearest_string, nearest.first),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            Spacer(Modifier.height(24.dp))

            Text(
                text = stringResource(R.string.tuner_hint),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun InTuneDot(inTune: Boolean) {
    val color = if (inTune) Color(0xFF4CAF50) else MaterialTheme.colorScheme.outlineVariant
    Canvas(modifier = Modifier.size(16.dp)) {
        drawCircle(color = color, radius = size.minDimension / 2)
    }
}

/**
 * Horizontal cents meter: -50 ¢ .. +50 ¢ with a green in-tune zone and a
 * needle that animates toward the current deviation.
 */
@Composable
private fun CentsMeter(
    cents: Float,
    active: Boolean,
    modifier: Modifier = Modifier
) {
    val clamped = cents.coerceIn(-50f, 50f)
    val needle by animateFloatAsState(
        targetValue = if (active) clamped else 0f,
        animationSpec = tween(120),
        label = "needle"
    )

    val trackColor = MaterialTheme.colorScheme.outlineVariant
    val greenColor = Color(0xFF4CAF50)
    val tickColor = MaterialTheme.colorScheme.outline
    val needleColor = if (!active) MaterialTheme.colorScheme.outlineVariant
    else if (abs(clamped) <= IN_TUNE_CENTS) greenColor
    else MaterialTheme.colorScheme.onSurface

    Canvas(modifier = modifier) {
        val trackStroke = Stroke(width = 8.dp.toPx(), cap = StrokeCap.Round)
        val centerY = size.height * 0.5f
        val trackWidth = size.width * 0.85f
        val startX = (size.width - trackWidth) / 2

        // Full track
        drawLine(
            color = trackColor,
            start = Offset(startX, centerY),
            end = Offset(startX + trackWidth, centerY),
            strokeWidth = trackStroke.width,
            cap = StrokeCap.Round
        )

        // In-tune zone (±5 cents)
        val zoneFraction = IN_TUNE_CENTS / 50f
        val center = startX + trackWidth / 2
        val zoneHalf = trackWidth / 2 * zoneFraction
        drawLine(
            color = greenColor,
            start = Offset(center - zoneHalf, centerY),
            end = Offset(center + zoneHalf, centerY),
            strokeWidth = trackStroke.width,
            cap = StrokeCap.Round
        )

        // Tick marks at ±50 and ±25
        val tickTop = centerY - 14.dp.toPx()
        val tickBottom = centerY - 6.dp.toPx()
        for (c in listOf(-50f, -25f, 25f, 50f)) {
            val x = center + trackWidth / 2 * (c / 50f)
            drawLine(
                color = tickColor,
                start = Offset(x, tickTop),
                end = Offset(x, tickBottom),
                strokeWidth = 2.dp.toPx(),
                cap = StrokeCap.Round
            )
        }

        // Needle: triangle pointer above the track
        val needleX = center + trackWidth / 2 * (needle / 50f)
        val needleHeight = 18.dp.toPx()
        val needleWidth = 9.dp.toPx()
        drawPath(
            path = androidx.compose.ui.graphics.Path().apply {
                moveTo(needleX, centerY - 6.dp.toPx())              // tip
                lineTo(needleX - needleWidth / 2, centerY - 6.dp.toPx() - needleHeight)
                lineTo(needleX + needleWidth / 2, centerY - 6.dp.toPx() - needleHeight)
                close()
            },
            color = needleColor
        )
    }
}