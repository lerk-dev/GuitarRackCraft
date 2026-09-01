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

import junit.framework.TestCase.assertEquals
import junit.framework.TestCase.assertFalse
import junit.framework.TestCase.assertNull
import junit.framework.TestCase.assertTrue
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okio.Buffer
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/** 纯内存令牌存储，替代 Android Keystore 依赖的 TokenManager。 */
private class FakeTokenStore : TokenStore {
    override var accessToken: String? = null
    override var refreshToken: String? = null
    override fun clear() { accessToken = null; refreshToken = null }
    override fun hasTokens(): Boolean = accessToken != null
}

class Tone3000ApiTest {

    @get:Rule
    val tmpFolder = TemporaryFolder()

    private lateinit var server: MockWebServer
    private lateinit var tokenStore: FakeTokenStore
    private lateinit var api: Tone3000Api

    @Before
    fun setUp() {
        server = MockWebServer()
        server.start()
        tokenStore = FakeTokenStore()
        api = Tone3000Api(tokenStore, baseUrl = server.url("/api/v1").toString())
    }

    @After
    fun tearDown() {
        server.shutdown()
    }

    private fun json(body: String) = MockResponse()
        .setHeader("Content-Type", "application/json")
        .setBody(body)

    @Test
    fun exchangeApiKeyParsesSession() {
        server.enqueue(json("""{"access_token":"at-1","refresh_token":"rt-1","expires_in":3600,"token_type":"Bearer"}"""))

        val session = api.exchangeApiKey("my-api-key")

        assertEquals("at-1", session?.access_token)
        assertEquals("rt-1", session?.refresh_token)

        val recorded = server.takeRequest()
        assertEquals("/api/v1/auth/session", recorded.path)
        // 登录请求走 bareClient，不应带 Authorization 头
        assertNull(recorded.getHeader("Authorization"))
        assertTrue(recorded.body.readUtf8().contains("my-api-key"))
    }

    @Test
    fun exchangeApiKeyThrowsOnHttpError() {
        server.enqueue(MockResponse().setResponseCode(401).setBody("unauthorized"))

        var thrown: ApiException? = null
        try { api.exchangeApiKey("bad") } catch (e: ApiException) { thrown = e }
        assertEquals(401, thrown?.code)
    }

    @Test
    fun searchTonesParsesPaginatedWrapper() {
        server.enqueue(json("""
            {"data":{"data":[
                {"id":"t1","title":"JCM 800","url":"https://x/t1","models_count":3},
                {"id":"t2","title":"5150","url":"https://x/t2","models_count":5}
            ],"page":1,"page_size":10,"total":2,"total_pages":1}}
        """.trimIndent()))

        val result = api.searchTones(query = "amp")

        assertEquals(2, result?.data?.size)
        assertEquals("JCM 800", result?.data?.first()?.title)
        assertEquals(1, result?.page)
        assertEquals(2, result?.total)

        val recorded = server.takeRequest()
        assertTrue(recorded.path!!.startsWith("/api/v1/tones/search"))
        assertTrue(recorded.path!!.contains("query=amp"))
        assertTrue(recorded.path!!.contains("page=1"))
    }

    @Test
    fun getUserRefreshesStaleTokenOn401() {
        tokenStore.accessToken = "stale-at"
        tokenStore.refreshToken = "valid-rt"

        // 1) 带 stale token 的请求被拒 2) 刷新成功 3) 重试成功
        server.enqueue(MockResponse().setResponseCode(401).setBody("token expired"))
        server.enqueue(json("""{"access_token":"fresh-at","refresh_token":"fresh-rt","expires_in":3600,"token_type":"Bearer"}"""))
        server.enqueue(json("""{"data":{"id":"u1","username":"guitarist","url":"https://x/u1"}}"""))

        val user = api.getUser()

        assertEquals("guitarist", user?.username)
        // 令牌已被 authenticator 刷新并写回存储
        assertEquals("fresh-at", tokenStore.accessToken)
        assertEquals("fresh-rt", tokenStore.refreshToken)

        val original = server.takeRequest()
        assertEquals("/api/v1/user", original.path)
        assertEquals("Bearer stale-at", original.getHeader("Authorization"))

        val refresh = server.takeRequest()
        assertEquals("/api/v1/auth/session/refresh", refresh.path)
        // 刷新请求走 bareClient，无 Authorization 头
        assertNull(refresh.getHeader("Authorization"))

        val retry = server.takeRequest()
        assertEquals("/api/v1/user", retry.path)
        assertEquals("Bearer fresh-at", retry.getHeader("Authorization"))
    }

    @Test
    fun downloadFileWritesAtomically() {
        val payload = "RIFF-FAKE-WAV-BYTES"
        server.enqueue(MockResponse().setResponseCode(200).setBody(Buffer().writeUtf8(payload)))
        val dest = File(tmpFolder.root, "model.nam")

        val ok = api.downloadFile(server.url("/files/model.nam").toString(), dest)

        assertTrue(ok)
        assertEquals(payload, dest.readText())
        // 临时 .part 文件不应残留
        assertFalse(File(tmpFolder.root, "model.nam.part").exists())
    }

    @Test
    fun downloadFileFailsCleanlyOn404() {
        server.enqueue(MockResponse().setResponseCode(404).setBody("not found"))
        val dest = File(tmpFolder.root, "missing.nam")

        val ok = api.downloadFile(server.url("/files/missing.nam").toString(), dest)

        assertFalse(ok)
        assertFalse(dest.exists())
        assertFalse(File(tmpFolder.root, "missing.nam.part").exists())
    }

    @Test
    fun downloadFileReportsProgress() {
        // 300KB 足以跨越多次节流阈值（64KB）
        val payload = "x".repeat(300 * 1024)
        server.enqueue(MockResponse().setResponseCode(200)
            .setHeader("Content-Length", payload.length)
            .setBody(Buffer().writeUtf8(payload)))
        val dest = File(tmpFolder.root, "big.nam")
        val reports = mutableListOf<Pair<Long, Long>>()

        val ok = api.downloadFile(server.url("/files/big.nam").toString(), dest) { done, total ->
            reports.add(done to total)
        }

        assertTrue(ok)
        // 最终上报精确到总长，且带正确的 content-length
        assertEquals(payload.length.toLong(), reports.last().first)
        assertEquals(payload.length.toLong(), reports.last().second)
        // 进度单调不减，且不止一次上报
        assertTrue(reports.size > 1)
        assertTrue(reports.zipWithNext().all { (a, b) -> b.first >= a.first })
    }
}
