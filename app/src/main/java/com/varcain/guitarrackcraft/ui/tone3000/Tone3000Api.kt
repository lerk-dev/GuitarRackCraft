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

import android.util.Log
import com.google.gson.Gson
import com.google.gson.reflect.TypeToken
import com.varcain.guitarrackcraft.BuildConfig
import okhttp3.*
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException
import java.util.concurrent.TimeUnit

class ApiException(val code: Int, val errorBody: String?) : Exception("API Error $code: $errorBody")

class Tone3000Api(
    private val tokenManager: TokenStore,
    private val baseUrl: String = "https://www.tone3000.com/api/v1"
) {
    private val gson = Gson()
    private val tag = "Tone3000Api"
    private val userAgent = "GuitarRackCraft/${BuildConfig.VERSION_NAME}"
    private val appId = "GuitarRackCraft"

    private val client = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .writeTimeout(30, TimeUnit.SECONDS)
        .callTimeout(120, TimeUnit.SECONDS)
        .addInterceptor { chain ->
            val requestBuilder = chain.request().newBuilder()
            requestBuilder.header("User-Agent", userAgent)
            requestBuilder.header("X-App-Id", appId)

            tokenManager.accessToken?.let { token ->
                if (token.isNotEmpty()) {
                    requestBuilder.header("Authorization", "Bearer $token")
                }
            }
            chain.proceed(requestBuilder.build())
        }
        .authenticator(object : Authenticator {
            override fun authenticate(route: Route?, response: Response): Request? {
                if (response.count401() > 2) {
                    Log.w(tag, "Too many 401s, giving up")
                    return null
                }

                val refreshToken = tokenManager.refreshToken ?: return null
                val accessToken = tokenManager.accessToken ?: return null

                Log.d(tag, "Attempting token refresh")
                synchronized(this) {
                    if (tokenManager.accessToken != accessToken) {
                        return response.request.newBuilder()
                            .header("Authorization", "Bearer ${tokenManager.accessToken}")
                            .build()
                    }

                    val newSession = try {
                        refreshSession(refreshToken, accessToken)
                    } catch (e: Exception) {
                        null
                    }
                    if (newSession != null) {
                        Log.i(tag, "Token refreshed successfully")
                        tokenManager.accessToken = newSession.access_token
                        tokenManager.refreshToken = newSession.refresh_token
                        return response.request.newBuilder()
                            .header("Authorization", "Bearer ${newSession.access_token}")
                            .build()
                    }
                }
                Log.e(tag, "Token refresh failed")
                return null
            }
        })
        .build()

    /** 无鉴权客户端：与主 client 共享连接池/线程池，但不附加
     *  Authorization 头和 401 刷新逻辑（用于登录与刷新本身）。 */
    private val bareClient: OkHttpClient = client.newBuilder()
        .apply {
            interceptors().clear()
            // OkHttp Kotlin 化后 authenticator(=null) 不再接受 null；
            // 用无操作的 authenticator 达到同样效果（永不触发重试）。
            authenticator { _, _ -> null }
        }
        .build()

    private fun Response.count401(): Int {
        var result = 1
        var r = priorResponse
        while (r != null) {
            result++
            r = r.priorResponse
        }
        return result
    }

    @Throws(ApiException::class, IOException::class)
    fun exchangeApiKey(apiKey: String): Session? {
        val json = gson.toJson(AuthRequest(apiKey))
        val body = json.toRequestBody("application/json".toMediaType())
        val request = Request.Builder()
            .url("$baseUrl/auth/session")
            .header("User-Agent", userAgent)
            .header("X-App-Id", appId)
            .post(body)
            .build()

        return try {
            bareClient.newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                if (!response.isSuccessful) {
                    Log.e(tag, "exchangeApiKey failed: ${response.code}")
                    throw ApiException(response.code, responseBody)
                }
                gson.fromJson(responseBody, Session::class.java)
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "exchangeApiKey exception", e)
            throw e
        }
    }

    @Throws(ApiException::class, IOException::class)
    private fun refreshSession(refreshToken: String, accessToken: String): Session? {
        val json = gson.toJson(RefreshRequest(refreshToken, accessToken))
        val body = json.toRequestBody("application/json".toMediaType())
        val request = Request.Builder()
            .url("$baseUrl/auth/session/refresh")
            .header("User-Agent", userAgent)
            .header("X-App-Id", appId)
            .post(body)
            .build()

        return try {
            bareClient.newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                if (!response.isSuccessful) {
                    Log.e(tag, "refreshSession failed: ${response.code}")
                    throw ApiException(response.code, responseBody)
                }
                gson.fromJson(responseBody, Session::class.java)
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "refreshSession exception", e)
            throw e
        }
    }

    @Throws(ApiException::class, IOException::class)
    fun getUser(): User? {
        val request = Request.Builder().url("$baseUrl/user").build()
        return try {
            client.newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                if (!response.isSuccessful) {
                    Log.e(tag, "getUser failed: ${response.code} - $responseBody")
                    throw ApiException(response.code, responseBody)
                }
                val type = object : TypeToken<DataWrapper<User>>() {}.type
                val wrapper: DataWrapper<User> = gson.fromJson(responseBody, type)
                wrapper.data
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "getUser exception", e)
            throw e
        }
    }

    @Throws(ApiException::class, IOException::class)
    fun searchTones(
        query: String = "",
        page: Int = 1,
        pageSize: Int = 10,
        gear: String? = null,
        sizes: String? = null,
        format: String? = null,
        architecture: String? = null,
        calibrated: Boolean? = null,
        sort: String? = null
    ): PaginatedResponse<List<Tone>>? {
        val urlBuilder = "$baseUrl/tones/search".toHttpUrlOrNull()?.newBuilder() ?: return null
        if (query.isNotEmpty()) urlBuilder.addQueryParameter("query", query)
        urlBuilder.addQueryParameter("page", page.toString())
        urlBuilder.addQueryParameter("page_size", pageSize.toString())
        if (!gear.isNullOrEmpty()) urlBuilder.addQueryParameter("gears", normalizeGearFilter(gear))
        if (!sizes.isNullOrEmpty()) urlBuilder.addQueryParameter("sizes", sizes)
        if (!format.isNullOrEmpty()) urlBuilder.addQueryParameter("format", format)
        if (!architecture.isNullOrEmpty()) urlBuilder.addQueryParameter("architecture", architecture)
        if (calibrated != null) urlBuilder.addQueryParameter("calibrated", calibrated.toString())
        if (!sort.isNullOrEmpty()) urlBuilder.addQueryParameter("sort", sort)
        
        val request = Request.Builder().url(urlBuilder.build()).build()

        Log.d(tag, "Searching tones: ${request.url}")
        return try {
            client.newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                // 完整响应体可能携带会话相关信息，仅在 debug 构建输出
                if (BuildConfig.DEBUG) Log.d(tag, "searchTones response: $responseBody")
                if (!response.isSuccessful) {
                    Log.e(tag, "searchTones failed: ${response.code}")
                    throw ApiException(response.code, responseBody)
                }
                try {
                    val type = object : TypeToken<DataWrapper<PaginatedResponse<List<Tone>>>>() {}.type
                    val wrapper: DataWrapper<PaginatedResponse<List<Tone>>> = gson.fromJson(responseBody, type)
                    wrapper.data
                } catch (e: Exception) {
                    val type = object : TypeToken<PaginatedResponse<List<Tone>>>() {}.type
                    gson.fromJson(responseBody, type)
                }
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "searchTones exception", e)
            throw e
        }
    }

    @Throws(ApiException::class, IOException::class)
    fun getModels(toneId: String, pageSize: Int = 10, architecture: String? = null): List<Model>? {
        // 未指定架构时，服务器默认只返回 A1（legacy）模型；
        // 主动请求 A2/A1/custom 并合并，让用户能看到全部可用模型（A2 优先）。
        if (architecture == null) {
            val merged = mutableListOf<Model>()
            for (arch in listOf("2", "1", "custom")) {
                try {
                    val batch = fetchModelsPageAll(toneId, pageSize, arch) ?: continue
                    merged.addAll(batch)
                } catch (e: Exception) {
                    // 单个架构请求失败不应阻断其他架构，记录日志后继续
                    Log.w(tag, "getModels: architecture=$arch fetch failed: ${e.message}")
                }
            }
            return merged.distinctBy { it.id }.ifEmpty { null }
        }
        return fetchModelsPageAll(toneId, pageSize, architecture)
    }

    /** 拉取指定架构下某音色的全部分页模型。 */
    @Throws(ApiException::class, IOException::class)
    private fun fetchModelsPageAll(toneId: String, pageSize: Int, architecture: String): List<Model>? {
        val allModels = mutableListOf<Model>()
        var currentPage = 1
        var totalPages = 1

        do {
            val urlBuilder = "$baseUrl/models".toHttpUrlOrNull()?.newBuilder() ?: return null
            urlBuilder.addQueryParameter("tone_id", toneId)
            urlBuilder.addQueryParameter("page", currentPage.toString())
            urlBuilder.addQueryParameter("page_size", pageSize.toString())
            if (architecture.isNotEmpty()) {
                urlBuilder.addQueryParameter("architecture", architecture)
            }
            val request = Request.Builder().url(urlBuilder.build()).build()

            try {
                client.newCall(request).execute().use { response ->
                    val responseBody = response.body?.string()
                    if (!response.isSuccessful) {
                        Log.e(tag, "getModels failed (page $currentPage): ${response.code} - $responseBody")
                        throw ApiException(response.code, responseBody)
                    }
                    val type = object : TypeToken<PaginatedResponse<List<Model>>>() {}.type
                    val paginated: PaginatedResponse<List<Model>> = gson.fromJson(responseBody, type)
                    allModels.addAll(paginated.data)
                    totalPages = paginated.total_pages
                    currentPage++
                }
            } catch (e: Exception) {
                if (e is ApiException) throw e
                Log.e(tag, "getModels exception (page $currentPage)", e)
                throw e
            }
        } while (currentPage <= totalPages)

        return allModels
    }

    @Throws(ApiException::class, IOException::class)
    fun getToneFromUrl(toneUrl: String, architecture: String? = null): Tone? {
        val url = if (toneUrl.startsWith("http")) {
            toneUrl
        } else {
            val cleanToneUrl = if (toneUrl.startsWith("/")) toneUrl else "/$toneUrl"
            "$baseUrl$cleanToneUrl"
        }
        val finalUrl = url.toHttpUrlOrNull()?.newBuilder()?.apply {
            if (!architecture.isNullOrEmpty()) {
                addQueryParameter("architecture", architecture)
            }
        }?.build()?.toString() ?: url
        Log.d(tag, "getToneFromUrl: $finalUrl")
        val request = Request.Builder().url(finalUrl).build()
        
        return try {
            client.newCall(request).execute().use { response ->
                val responseBody = response.body?.string()
                if (!response.isSuccessful) {
                    Log.e(tag, "getToneFromUrl failed: ${response.code} - $responseBody")
                    throw ApiException(response.code, responseBody)
                }
                try {
                    val type = object : TypeToken<DataWrapper<Tone>>() {}.type
                    val wrapper: DataWrapper<Tone> = gson.fromJson(responseBody, type)
                    wrapper.data
                } catch (e: Exception) {
                    gson.fromJson(responseBody, Tone::class.java)
                }
            }
        } catch (e: Exception) {
            if (e is ApiException) throw e
            Log.e(tag, "getToneFromUrl exception", e)
            throw e
        }
    }

    private fun normalizeGearFilter(gear: String): String =
        gear.split(",")
            .filter { it.isNotBlank() }
            .joinToString("-") { if (it == "full-rig") "amp-cab" else it }

    /**
     * 下载文件到 [destFile]。
     *
     * @param onProgress 进度回调（已读字节数, 总字节数；总长未知时为 -1），
     *  在 IO 线程回调，节流为最多每 256KB 或每 1% 触发一次。
     */
    fun downloadFile(
        url: String,
        destFile: java.io.File,
        onProgress: ((Long, Long) -> Unit)? = null
    ): Boolean {
        val request = Request.Builder().url(url).build()
        // 先写临时文件再原子重命名，避免中断后留下损坏的半截文件
        val tmpFile = java.io.File(destFile.parentFile, destFile.name + ".part")
        return try {
            client.newCall(request).execute().use { response ->
                if (!response.isSuccessful) {
                    Log.e(tag, "downloadFile failed: ${response.code}")
                    return false
                }
                val total = response.body?.contentLength() ?: -1L
                val body = response.body ?: return false
                // 注意：不要把 use{} 结果接到 ?: 上——内部块以可空调用结尾时会
                // 整体返回 null 误触发 return false。use 作为独立语句使用。
                body.byteStream().use { input ->
                    tmpFile.outputStream().use { output ->
                        val buf = ByteArray(64 * 1024)
                        var read: Int
                        var done = 0L
                        var lastReported = -1L
                        while (input.read(buf).also { read = it } != -1) {
                            output.write(buf, 0, read)
                            done += read
                            if (onProgress != null) {
                                // 节流：每 256KB 或总进度每变化 1% 上报一次
                                val step = if (total > 0) total / 100 else 256 * 1024
                                val threshold = maxOf(step, 64 * 1024L)
                                if (done - lastReported >= threshold) {
                                    onProgress(done, total)
                                    lastReported = done
                                }
                            }
                        }
                        onProgress?.invoke(done, total)
                    }
                }
            }
            if (tmpFile.exists() && tmpFile.length() > 0) {
                if (destFile.exists()) destFile.delete()
                if (!tmpFile.renameTo(destFile)) {
                    // 跨文件系统重命名失败时退回复制
                    tmpFile.copyTo(destFile, overwrite = true)
                    tmpFile.delete()
                }
                true
            } else {
                tmpFile.delete()
                false
            }
        } catch (e: Exception) {
            Log.e(tag, "downloadFile exception", e)
            tmpFile.delete()
            false
        }
    }
}
