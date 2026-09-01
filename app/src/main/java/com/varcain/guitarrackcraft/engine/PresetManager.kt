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
import android.util.Base64
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/**
 * Manages preset save/load using JSON files stored in filesDir/presets/.
 * Each preset captures the full chain state (all plugins' control ports + state properties).
 */
class PresetManager(private val engine: NativeEngine) {

    companion object {
        private const val TAG = "PresetManager"
        private const val PRESETS_DIR = "presets"
        private const val ASSET_PRESETS_DIR = "presets"
        private const val NEURAL_MODELS_DIR = "neural_models"
        private const val PREFS_NAME = "preset_manager"
        private const val KEY_BUNDLED_COPIED = "bundled_presets_copied"
        private const val KEY_NEURAL_MODELS_COPIED = "neural_models_copied"
        // 内置预设内容版本：每次改动 assets/presets/ 中的预设时递增，
        // 版本变化时强制用新版内置预设覆盖设备上的旧版内置预设。
        private const val BUNDLED_PRESETS_VERSION = 2
        private const val KEY_BUNDLED_VERSION = "bundled_presets_version"

        /**
         * 首次启动时把 assets/presets/ 下内置的名曲预设复制到 presets 目录。
         * 默认只复制一次且不覆盖用户已有的同名文件；但内置预设版本号变化时，
         * 会强制用 assets 中的新版预设覆盖旧版（用于将 gx_amp 音色链升级为 NAM 音色链等）。
         */
        fun importBundledPresets(context: Context) {
            try {
                val targetDir = File(context.filesDir, PRESETS_DIR)
                targetDir.mkdirs()
                val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                val copied = prefs.getStringSet(KEY_BUNDLED_COPIED, emptySet())!!.toMutableSet()
                val prevVersion = prefs.getInt(KEY_BUNDLED_VERSION, 0)
                val forceRefresh = prevVersion != BUNDLED_PRESETS_VERSION

                val names = context.assets.list(ASSET_PRESETS_DIR)?.toList() ?: return
                for (name in names) {
                    if (!name.endsWith(".json")) continue
                    val target = File(targetDir, name)
                    if (forceRefresh) {
                        // 版本升级：覆盖旧版内置预设
                        context.assets.open("$ASSET_PRESETS_DIR/$name").use { input ->
                            FileOutputStream(target).use { output -> input.copyTo(output) }
                        }
                        copied.add(name)
                        continue
                    }
                    if (name in copied) continue
                    if (target.exists()) {
                        // 用户已有同名文件（可能自己改过），不覆盖
                        copied.add(name)
                        continue
                    }
                    context.assets.open("$ASSET_PRESETS_DIR/$name").use { input ->
                        FileOutputStream(target).use { output -> input.copyTo(output) }
                    }
                    copied.add(name)
                }
                if (forceRefresh) {
                    prefs.edit().putInt(KEY_BUNDLED_VERSION, BUNDLED_PRESETS_VERSION).apply()
                    Log.i(TAG, "importBundledPresets: bundled presets version upgraded to $BUNDLED_PRESETS_VERSION")
                }
                prefs.edit().putStringSet(KEY_BUNDLED_COPIED, copied).apply()
                Log.i(TAG, "importBundledPresets: imported bundled presets into ${targetDir.absolutePath}")
            } catch (e: Exception) {
                Log.e(TAG, "importBundledPresets failed: ${e.message}", e)
            }
        }

        /**
         * 首次启动时把 assets/neural_models/ 下内置的 NAM 模型复制到
         * filesDir/neural_models/（预设中的 NAM 插件从这里加载模型）。
         * 只复制一次且不覆盖用户自行下载的同名模型。
         */
        fun importBundledNeuralModels(context: Context) {
            try {
                val targetDir = File(context.filesDir, NEURAL_MODELS_DIR)
                targetDir.mkdirs()
                val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                val copied = prefs.getStringSet(KEY_NEURAL_MODELS_COPIED, emptySet())!!.toMutableSet()

                val names = context.assets.list(NEURAL_MODELS_DIR)?.toList() ?: return
                for (name in names) {
                    if (name in copied) continue
                    val target = File(targetDir, name)
                    if (target.exists()) {
                        // 已有同名文件（可能是用户自己下载的），不覆盖
                        copied.add(name)
                        continue
                    }
                    context.assets.open("$NEURAL_MODELS_DIR/$name").use { input ->
                        FileOutputStream(target).use { output -> input.copyTo(output) }
                    }
                    copied.add(name)
                }
                prefs.edit().putStringSet(KEY_NEURAL_MODELS_COPIED, copied).apply()
                Log.i(TAG, "importBundledNeuralModels: imported bundled models into ${targetDir.absolutePath}")
            } catch (e: Exception) {
                Log.e(TAG, "importBundledNeuralModels failed: ${e.message}", e)
            }
        }
    }

    private fun presetsDir(context: Context): File {
        val dir = File(context.filesDir, PRESETS_DIR)
        if (!dir.exists()) dir.mkdirs()
        return dir
    }

    /**
     * Save the current chain state as a named preset.
     * @return true if saved successfully.
     */
    fun savePreset(context: Context, name: String): Boolean {
        val stateJson = engine.saveChainState()
        if (stateJson == null) {
            Log.e(TAG, "savePreset: nativeSaveChainState returned null")
            return false
        }

        // Parse the native JSON and add metadata
        val root = JSONObject(stateJson)
        root.put("presetName", name)
        root.put("timestamp", System.currentTimeMillis())

        // Collect plugin URIs for quick identification
        val plugins = root.optJSONArray("plugins")
        if (plugins != null) {
            val uris = JSONArray()
            for (i in 0 until plugins.length()) {
                uris.put(plugins.getJSONObject(i).optString("uri", ""))
            }
            root.put("pluginUris", uris)
        }

        val file = File(presetsDir(context), "$name.json")
        file.writeText(root.toString(2))
        Log.i(TAG, "savePreset: saved '$name' to ${file.absolutePath}")
        return true
    }

    /**
     * Load a preset by name: reads the file and delegates to [loadPresetFromJson].
     * @return true if all plugins restored successfully.
     */
    fun loadPreset(context: Context, name: String): Boolean {
        val file = File(presetsDir(context), "$name.json")
        if (!file.exists()) {
            Log.e(TAG, "loadPreset: file not found: ${file.absolutePath}")
            return false
        }
        return loadPresetFromJson(file.readText())
    }

    /**
     * Load a preset from a raw JSON string: clears the current rack, adds plugins by URI,
     * then restores their control port values and state properties.
     * @return true if all plugins restored successfully.
     */
    fun loadPresetFromJson(json: String): Boolean {
        val root = JSONObject(json)
        val plugins = root.optJSONArray("plugins")
        if (plugins == null) {
            Log.e(TAG, "loadPresetFromJson: no plugins array in preset")
            return false
        }

        // Clear current rack (remove in reverse order to keep indices valid)
        val rackSize = engine.getRackSize()
        for (i in (rackSize - 1) downTo 0) {
            engine.removePluginFromRack(i)
        }

        // Add each plugin by URI. The native engine accepts "FORMAT:id":
        //   LV2  → "LV2:<lv2-uri>"
        //   VST2 → "VST2:<uuid>"
        //   VST3 → "VST3:<uuid>"
        // Legacy presets (pre-2026-05-26) wrote only "uri" without a "format"
        // field — back then only LV2 was supported, so default to "LV2".
        for (i in 0 until plugins.length()) {
            val pluginObj = plugins.getJSONObject(i)
            val uri = pluginObj.optString("uri", "")
            val format = pluginObj.optString("format", "").ifEmpty { "LV2" }
            if (uri.isEmpty()) {
                Log.e(TAG, "loadPresetFromJson: plugin[$i] has no URI")
                return false
            }
            val fullId = "$format:$uri"
            val pos = engine.addPluginToRack(fullId, -1)
            if (pos < 0) {
                Log.e(TAG, "loadPresetFromJson: failed to add plugin '$fullId'")
                return false
            }
        }

        // Restore parameters for each plugin
        var allOk = true
        for (i in 0 until plugins.length()) {
            val pluginObj = plugins.getJSONObject(i)
            if (!restorePluginFromJson(i, pluginObj)) {
                allOk = false
            }
        }

        Log.i(TAG, "loadPresetFromJson: restored, allOk=$allOk")
        return allOk
    }

    /**
     * List all saved presets.
     * @return list of preset names (without .json extension).
     */
    fun listPresets(context: Context): List<String> {
        val dir = presetsDir(context)
        return dir.listFiles { f -> f.extension == "json" }
            ?.map { it.nameWithoutExtension }
            ?.sorted()
            ?: emptyList()
    }

    /**
     * Delete a saved preset.
     * @return true if deleted.
     */
    fun deletePreset(context: Context, name: String): Boolean {
        val file = File(presetsDir(context), "$name.json")
        val ok = file.delete()
        Log.i(TAG, "deletePreset: '$name' deleted=$ok")
        return ok
    }

    fun getPresetJson(context: Context, name: String): String? {
        val file = File(presetsDir(context), "$name.json")
        return if (file.exists()) file.readText() else null
    }

    private fun restorePluginFromJson(pluginIndex: Int, pluginObj: JSONObject): Boolean {
        // Control ports
        val controlPortsArr = pluginObj.optJSONArray("controlPorts")
        val portCount = controlPortsArr?.length() ?: 0
        val portValues = FloatArray(portCount)
        val portIndices = IntArray(portCount)
        for (i in 0 until portCount) {
            val cp = controlPortsArr!!.getJSONObject(i)
            portIndices[i] = cp.getInt("index")
            portValues[i] = cp.getDouble("value").toFloat()
        }

        // State properties
        val propsArr = pluginObj.optJSONArray("stateProperties")
        val propCount = propsArr?.length() ?: 0
        val propKeys = Array(propCount) { "" }
        val propTypes = Array(propCount) { "" }
        val propValues = Array(propCount) { ByteArray(0) }
        val propFlags = IntArray(propCount)
        for (i in 0 until propCount) {
            val prop = propsArr!!.getJSONObject(i)
            propKeys[i] = prop.getString("key")
            propTypes[i] = prop.getString("type")
            propFlags[i] = prop.optInt("flags", 0)

            val encoding = prop.optString("encoding", "")
            val valueStr = prop.optString("value", "")
            propValues[i] = if (encoding == "base64") {
                Base64.decode(valueStr, Base64.DEFAULT)
            } else {
                // String value — add null terminator for LV2 compatibility
                (valueStr + "\u0000").toByteArray(Charsets.UTF_8)
            }
        }

        return engine.restorePluginState(
            pluginIndex, portValues, portIndices,
            propKeys, propTypes, propValues, propFlags
        )
    }
}
