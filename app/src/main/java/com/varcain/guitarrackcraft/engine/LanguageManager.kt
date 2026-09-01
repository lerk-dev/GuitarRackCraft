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
import android.content.res.Configuration
import java.util.Locale

/**
 * 应用内语言管理：跟随系统 / English / 简体中文。
 *
 * 语言选择持久化到 SharedPreferences；MainActivity.attachBaseContext()
 * 通过 [wrapContext] 应用所选语言，因此 stringResource 等资源解析会自动
 * 使用目标语言。Android 13+ 同时写入系统 LocaleManager，保证系统层面一致。
 */
object LanguageManager {

    private const val PREFS_NAME = "language_prefs"
    private const val KEY_LANG = "app_language"

    /** 跟随系统 */
    const val LANG_SYSTEM = "system"
    /** English */
    const val LANG_EN = "en"
    /** 简体中文 */
    const val LANG_ZH = "zh"

    /** 当前选择的语言标识（system / en / zh）。 */
    fun getLanguage(context: Context): String =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(KEY_LANG, LANG_SYSTEM) ?: LANG_SYSTEM

    /** 保存语言选择。 */
    fun setLanguage(context: Context, language: String) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putString(KEY_LANG, language).apply()
    }

    /** 返回所选语言对应的 Locale；system 模式返回系统默认语言。 */
    fun getLocale(context: Context): Locale = when (getLanguage(context)) {
        LANG_EN -> Locale.ENGLISH
        LANG_ZH -> Locale.SIMPLIFIED_CHINESE
        else -> Locale.getDefault()
    }

    /**
     * 把 context 包装为所选语言，供 Activity.attachBaseContext 使用。
     * system 模式直接返回原 context（不重复包装）。
     */
    fun wrapContext(base: Context): Context {
        val language = getLanguage(base)
        if (language == LANG_SYSTEM) return base
        val config = Configuration(base.resources.configuration)
        config.setLocale(getLocale(base))
        return base.createConfigurationContext(config)
    }
}
