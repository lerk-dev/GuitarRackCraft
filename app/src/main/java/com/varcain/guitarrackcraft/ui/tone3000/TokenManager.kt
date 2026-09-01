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

package com.varcain.guitarrackcraft.ui.tone3000

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

/**
 * 令牌存取抽象：Tone3000Api 依赖此接口而非具体实现，便于单元测试注入。
 */
interface TokenStore {
    var accessToken: String?
    var refreshToken: String?
    fun clear()
    fun hasTokens(): Boolean
}

/**
 * Tone3000 会话令牌存储。
 *
 * 令牌保存在 EncryptedSharedPreferences 中（AES-256-GCM，密钥由 Android
 * Keystore 持有）。首次启动时会自动把旧版明文 SharedPreferences 中的
 * 令牌迁移到加密存储并删除明文文件。
 */
class TokenManager(context: Context) : TokenStore {
    private val prefs: SharedPreferences = create(context.applicationContext)

    override var accessToken: String?
        get() = prefs.getString(KEY_ACCESS_TOKEN, null)
        set(value) = prefs.edit().putString(KEY_ACCESS_TOKEN, value).apply()

    override var refreshToken: String?
        get() = prefs.getString(KEY_REFRESH_TOKEN, null)
        set(value) = prefs.edit().putString(KEY_REFRESH_TOKEN, value).apply()

    override fun clear() {
        prefs.edit().remove(KEY_ACCESS_TOKEN).remove(KEY_REFRESH_TOKEN).apply()
    }

    override fun hasTokens(): Boolean = accessToken != null

    private fun create(context: Context): SharedPreferences {
        // 1) 读取旧版明文令牌（若文件已是加密格式，明文读取拿不到键 → 返回 null）
        val legacy = readLegacyPlaintext(context)
        if (legacy != null) {
            // 删除明文文件，让加密实例从头创建
            context.deleteSharedPreferences(PREFS_NAME)
        }

        // 2) 打开加密 prefs；Keystore 偶发损坏时重建一次，仍失败则退回明文
        val store = try {
            openEncrypted(context)
        } catch (e: Exception) {
            Log.e(TAG, "EncryptedSharedPreferences unavailable, resetting store", e)
            context.deleteSharedPreferences(PREFS_NAME)
            try {
                openEncrypted(context)
            } catch (e2: Exception) {
                Log.e(TAG, "Falling back to plaintext prefs", e2)
                context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            }
        }

        // 3) 把明文时代的令牌写回加密存储
        if (legacy != null) {
            store.edit()
                .putString(KEY_ACCESS_TOKEN, legacy.first)
                .putString(KEY_REFRESH_TOKEN, legacy.second)
                .apply()
            Log.i(TAG, "Migrated legacy plaintext tokens to encrypted storage")
        }
        return store
    }

    private fun openEncrypted(context: Context): SharedPreferences {
        val masterKey = MasterKey.Builder(context)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        return EncryptedSharedPreferences.create(
            context,
            PREFS_NAME,
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

    /** 返回 (accessToken, refreshToken)；无明文令牌时返回 null。 */
    private fun readLegacyPlaintext(context: Context): Pair<String?, String?>? {
        return try {
            val legacy = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            val access = legacy.getString(KEY_ACCESS_TOKEN, null)
            val refresh = legacy.getString(KEY_REFRESH_TOKEN, null)
            if (access == null && refresh == null) null else Pair(access, refresh)
        } catch (e: Exception) {
            Log.w(TAG, "Legacy token read skipped: ${e.message}")
            null
        }
    }

    companion object {
        private const val TAG = "TokenManager"
        private const val PREFS_NAME = "tone3000_prefs"
        private const val KEY_ACCESS_TOKEN = "access_token"
        private const val KEY_REFRESH_TOKEN = "refresh_token"
    }
}
