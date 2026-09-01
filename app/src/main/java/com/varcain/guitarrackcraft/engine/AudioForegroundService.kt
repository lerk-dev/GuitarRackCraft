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

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.varcain.guitarrackcraft.MainActivity
import com.varcain.guitarrackcraft.R

/**
 * 前台 Service：音频引擎运行期间保持前台优先级，
 * 避免切后台/锁屏后进程被冻结（Android 12+ 会冻结后台音频回调线程）。
 *
 * 生命周期完全跟随 AudioEngine：start() 成功后启动，stop() 时结束。
 * 通知提供"停止"动作，点击内容回到主界面。
 */
class AudioForegroundService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
            else -> {
                ServiceCompat.startForeground(
                    this,
                    NOTIFICATION_ID,
                    buildNotification(),
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
                    else 0
                )
            }
        }
        // 音频引擎由用户显式控制，被杀后不静默重启
        return START_NOT_STICKY
    }

    private fun createChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.audio_engine_channel_name),
            NotificationManager.IMPORTANCE_LOW  // 无声音，不出现在锁屏头部
        ).apply {
            description = getString(R.string.audio_engine_channel_desc)
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val contentIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val stopIntent = PendingIntent.getService(
            this, 1,
            Intent(this, AudioForegroundService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_guitar)
            .setContentTitle(getString(R.string.audio_engine_notification_title))
            .setContentText(getString(R.string.audio_engine_notification_text))
            .setContentIntent(contentIntent)
            .addAction(0, getString(R.string.audio_engine_action_stop), stopIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "audio_engine"
        private const val NOTIFICATION_ID = 1001
        private const val ACTION_STOP = "com.varcain.guitarrackcraft.action.STOP_AUDIO"

        /** 音频引擎启动成功后调用（须在应用前台时）。 */
        fun start(context: Context) {
            val intent = Intent(context, AudioForegroundService::class.java)
            try {
                ContextCompat.startForegroundService(context, intent)
            } catch (e: Exception) {
                android.util.Log.w("AudioFGS", "startForegroundService failed", e)
            }
        }

        /** 音频引擎停止/应用退出时调用。 */
        fun stop(context: Context) {
            try {
                context.startService(
                    Intent(context, AudioForegroundService::class.java).setAction(ACTION_STOP)
                )
            } catch (_: Exception) {
                // Service 未运行时 startService 会抛 IllegalStateException，忽略
            }
        }
    }
}
