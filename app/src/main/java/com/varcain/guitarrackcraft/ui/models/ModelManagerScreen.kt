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

package com.varcain.guitarrackcraft.ui.models

import android.content.Context
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.LibraryMusic
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.varcain.guitarrackcraft.R
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.guitarrackcraft.engine.RackManager
import com.varcain.guitarrackcraft.engine.X11Bridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

private data class ModelEntry(
    val file: File,
    val name: String,
    val sizeBytes: Long,
    val isBundled: Boolean
)

private const val KEY_IR_ENABLED = "ir_loading_enabled"

/** Directories managed by this screen. */
private val MANAGED_DIRS = listOf("neural_models", "aidax_models")

private val NAM_MODEL_URI = "http://github.com/mikeoliphant/neural-amp-modeler-lv2#model"
private val NEURALRACK_MODEL_URI = "urn:brummer:neuralrack#Neural_Model"
private val NEURALRACK_IR_URI = "urn:brummer:neuralrack#irfile"
private val IMPULSELOADER_IR_URI = "urn:brummer:ImpulseLoader#irfile"

private fun listModels(filesDir: File): List<ModelEntry> {
    val bundled = bundledModelNames()
    return MANAGED_DIRS.flatMap { dirName ->
        val dir = File(filesDir, dirName)
        if (!dir.isDirectory) return@flatMap emptyList()
        dir.walkTopDown()
            .filter { it.isFile && it.extension.lowercase() in setOf("nam", "json", "aidax", "wav") }
            .map { f ->
                ModelEntry(
                    file = f,
                    name = f.name,
                    sizeBytes = f.length(),
                    isBundled = f.parentFile?.name == "neural_models" && f.name in bundled
                )
            }
            .toList()
    }.sortedBy { it.name.lowercase() }
}

/** Names of the models bundled in assets — deletion of these needs a stronger warning. */
private fun bundledModelNames(): Set<String> = setOf(
    "Tim_R_JCM2000_Crunch.nam",
    "Tim_R_JCM2000_Clean.nam",
    "Tim_R_Fender_TwinVerb_Norm_Bright.nam",
    "Tim_R_Fender_TwinVerb_Vibrato_Bright.nam",
    "Mikhail_K_Sovtek_MIG50.nam",
    "Roman_A_LT_MESA_MARKIV_1.nam",
    "Sascha_S_DirtyShirleyMini_Clean_B1_M6_T7_MV10_G4.nam",
    "Helga_B_6505+_Red_ch_-_NoBoost.nam",
    "BOG UU II Gain BAL CAB.nam",
    "BNO JT45DR I Crunch BAL CAB3.nam",
    "MRSH JM50LD I Crunch2 FAT CAB.nam",
    "FNDR BFDRI VB Clean BAL2 CAB.nam",
    "Mars Gain 7.nam",
    "Ibanez TS9 Tube Screamer Drive 7 Tone 7 Level 7.nam",
    "Roland JC 120B Jazz Chorus_ Bright On, SM57.nam",
    "TKIMP RH v4 pos2.nam",
    "TWIN REVERB __ BALANCED.wav",
    "P10R UR 4x10 BU87IC 2.00in 1.0in.wav"
)

private fun formatSize(bytes: Long): String = when {
    bytes >= 1 shl 20 -> "%.1f MB".format(bytes / 1048576.0)
    bytes >= 1 shl 10 -> "%.0f KB".format(bytes / 1024.0)
    else -> "$bytes B"
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ModelManagerScreen(
    onNavigateBack: () -> Unit
) {
    val context = LocalContext.current
    val filesDir = context.filesDir
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }

    var models by remember { mutableStateOf(listModels(filesDir)) }
    var pendingDelete by remember { mutableStateOf<ModelEntry?>(null) }
    var feedback by remember { mutableStateOf<String?>(null) }
    var importedCount by remember { mutableStateOf(0) }

    // IR loading toggle. Some NAM files already include a cab (full rig), so
    // stacking an additional IR on top double-filters the tone and makes it
    // sound muffled/boxy. Persisted so the user's choice survives restarts.
    val prefs = context.getSharedPreferences("model_manager", Context.MODE_PRIVATE)
    var irEnabled by remember { mutableStateOf(prefs.getBoolean(KEY_IR_ENABLED, true)) }
    fun setIrEnabled(enabled: Boolean) {
        irEnabled = enabled
        prefs.edit().putBoolean(KEY_IR_ENABLED, enabled).apply()
    }

    // Surface feedback (apply/import result) via the snackbar
    LaunchedEffect(feedback) {
        feedback?.let {
            snackbar.showSnackbar(it)
            feedback = null
        }
    }

    // Reload the list after an import finished (importedCount changes even on failure=0)
    LaunchedEffect(importedCount) {
        if (importedCount > 0) {
            models = listModels(filesDir)
            snackbar.showSnackbar(context.getString(R.string.models_imported, importedCount))
            importedCount = 0
        }
    }

    // SAF file picker for importing models
    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris ->
        if (uris.isEmpty()) return@rememberLauncherForActivityResult
        scope.launch {
            val count = withContext(Dispatchers.IO) {
                var ok = 0
                for (uri in uris) {
                    val name = uri.lastPathSegment?.substringAfterLast('/') ?: "model.nam"
                    val target = File(filesDir, "neural_models/$name")
                    target.parentFile?.mkdirs()
                    try {
                        context.contentResolver.openInputStream(uri)?.use { input ->
                            target.outputStream().use { output -> input.copyTo(output) }
                        }
                        if (target.length() > 0) ok++
                    } catch (_: Exception) {
                        target.delete()
                    }
                }
                ok
            }
            if (count == 0) {
                feedback = context.getString(R.string.models_import_failed)
            } else {
                importedCount = count
            }
        }
    }

    BackHandler { onNavigateBack() }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.models_title)) },
                navigationIcon = {
                    IconButton(onClick = { onNavigateBack() }) {
                        Icon(Icons.Default.ArrowBack, contentDescription = stringResource(R.string.common_back))
                    }
                },
                actions = {
                    IconButton(onClick = {
                        importLauncher.launch(arrayOf("*/*"))
                    }) {
                        Icon(
                            Icons.Default.LibraryMusic,
                            contentDescription = stringResource(R.string.models_import)
                        )
                    }
                }
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = { importLauncher.launch(arrayOf("*/*")) },
                icon = { Icon(Icons.Default.LibraryMusic, contentDescription = null) },
                text = { Text(stringResource(R.string.models_import)) }
            )
        }
    ) { padding ->
        if (models.isEmpty()) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding),
                verticalArrangement = Arrangement.Center,
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text(
                    text = stringResource(R.string.models_empty),
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        } else {
            LazyColumn(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(
                    start = 16.dp, end = 16.dp, top = 8.dp, bottom = 88.dp
                ),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                item {
                    Card(modifier = Modifier.fillMaxWidth()) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 16.dp, vertical = 10.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column(modifier = Modifier.weight(1f)) {
                                Text(
                                    text = stringResource(R.string.models_ir_enabled),
                                    style = MaterialTheme.typography.bodyLarge,
                                    fontWeight = FontWeight.Medium
                                )
                                Text(
                                    text = stringResource(R.string.models_ir_enabled_hint),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                            Switch(
                                checked = irEnabled,
                                onCheckedChange = { setIrEnabled(it) },
                                colors = SwitchDefaults.colors(
                                    checkedThumbColor = MaterialTheme.colorScheme.primary
                                )
                            )
                        }
                    }
                }
                items(models, key = { it.file.absolutePath }) { model ->
                    ModelRow(
                        model = model,
                        onApply = {
                            if (model.file.extension.equals("wav", ignoreCase = true) && !irEnabled) {
                                feedback = context.getString(R.string.models_ir_disabled)
                                return@ModelRow
                            }
                            val ok = applyModelToRack(model)
                            feedback = if (ok) context.getString(R.string.models_applied, model.name)
                            else context.getString(R.string.models_no_plugin)
                        },
                        onDelete = { pendingDelete = model }
                    )
                }
            }
        }
    }

    // Delete confirmation
    pendingDelete?.let { model ->
        AlertDialog(
            onDismissRequest = { pendingDelete = null },
            title = { Text(stringResource(R.string.models_delete_title)) },
            text = {
                Text(
                    if (model.isBundled) stringResource(R.string.models_delete_bundled_text, model.name)
                    else stringResource(R.string.models_delete_text, model.name)
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    model.file.delete()
                    model.file.parentFile?.takeIf { it.isDirectory && it.listFiles()?.isEmpty() == true }?.delete()
                    models = listModels(filesDir)
                    pendingDelete = null
                }) {
                    Text(stringResource(R.string.common_delete), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { pendingDelete = null }) {
                    Text(stringResource(R.string.common_cancel))
                }
            }
        )
    }
}

/**
 * Load the model/IR file into the first compatible plugin in the rack.
 * .nam/.json/.aidax files go to the NAM model slot (neural-amp-modeler or
 * neuralrack); .wav impulse responses go to the IR slot (neuralrack's
 * irfile, or ImpulseLoader if present). Shows a snackbar-style feedback
 * via its own message flow handled by the caller.
 */
private fun applyModelToRack(model: ModelEntry): Boolean {
    val isIr = model.file.extension.equals("wav", ignoreCase = true)
    val rack = RackManager.getRackPlugins()
    val idx = rack.indexOfFirst {
        when {
            isIr -> it.id.contains("neuralrack", ignoreCase = true) ||
                it.id.contains("ImpulseLoader", ignoreCase = true)
            else -> it.id.contains("neural-amp-modeler") || it.id.contains("neuralrack", ignoreCase = true)
        }
    }
    if (idx < 0) return false
    val plugin = rack[idx]
    val uri = when {
        isIr && plugin.id.contains("ImpulseLoader", ignoreCase = true) -> IMPULSELOADER_IR_URI
        isIr -> NEURALRACK_IR_URI
        plugin.id.contains("neuralrack", ignoreCase = true) -> NEURALRACK_MODEL_URI
        else -> NAM_MODEL_URI
    }
    NativeEngine.getInstance().setPluginFilePath(idx, uri, model.file.absolutePath)
    X11Bridge.deliverFileToPluginUI(idx, uri, model.file.absolutePath)
    // Same buffered-mode enable as RackViewModel.setPluginFilePath: Neuralrack
    // infers NAM on the audio callback thread by default; heavy models overrun
    // the callback budget on mid-range SoCs -> crackling. Buffered port (20)
    // moves inference to a background thread (cost: +1 block ~4 ms).
    if (!isIr && uri == NEURALRACK_MODEL_URI) {
        RackManager.setParameter(idx, 20, 1f)
        // Input normalization for Slot A: lifts the weak pickup signal to the
        // model's working level (see RackViewModel.setPluginFilePath).
        RackManager.setParameter(idx, 12, 1f)
    }
    RackManager.notifyModelLoaded(idx, model.file.nameWithoutExtension)
    return true
}

@Composable
private fun ModelRow(
    model: ModelEntry,
    onApply: () -> Unit,
    onDelete: () -> Unit
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = model.name,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        text = formatSize(model.sizeBytes),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        text = model.file.parentFile?.name ?: "",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    if (model.isBundled) {
                        Text(
                            text = "· ${stringResource(R.string.models_bundled)}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.primary
                        )
                    }
                }
            }
            IconButton(
                onClick = onApply,
                modifier = Modifier.size(40.dp)
            ) {
                Icon(
                    Icons.Default.PlayArrow,
                    contentDescription = stringResource(R.string.models_apply),
                    tint = MaterialTheme.colorScheme.primary
                )
            }
            IconButton(
                onClick = onDelete,
                modifier = Modifier.size(40.dp)
            ) {
                Icon(
                    Icons.Default.Delete,
                    contentDescription = stringResource(R.string.common_delete),
                    tint = MaterialTheme.colorScheme.error
                )
            }
        }
    }
}
