package com.jitong.im.data

import com.tencent.mmkv.MMKV

/**
 * KV 凭证存储（MMKV，对齐 QQNT 双存储：MMKV 管 KV/凭证，Room 管消息）。
 * “记住账号密码”：勾选后持久化手机号 + 密码（明文，仅用于登录页回填），
 * 只要勾选着，退出登录/被踢/重开 App 都不清空；不勾选则不保存。
 * 注意：不做自动登录，回填后仍需用户手动点登录。
 */
object Prefs {
    private val kv: MMKV by lazy { MMKV.defaultMMKV() }

    var tel: String?
        get() = kv.decodeString("tel", null)?.takeIf { it.isNotEmpty() }
        set(v) {
            if (v == null) kv.removeValueForKey("tel") else kv.encode("tel", v)
        }

    /** 记住的明文密码（用于登录页回填；仅本地 MMKV 存储） */
    var pass: String?
        get() = kv.decodeString("pass", null)?.takeIf { it.isNotEmpty() }
        set(v) {
            if (v == null) kv.removeValueForKey("pass") else kv.encode("pass", v)
        }

    /** 是否记住账号密码（登录页勾选框持久化） */
    var remember: Boolean
        get() = kv.decodeBool("remember", false)
        set(v) = kv.encode("remember", v).let {}

    /**
     * 清除保存的账号密码。仅在“未勾选记住”时才真正清除；
     * 勾选了记住则保留（退出登录/被踢/重开都不清空）。
     */
    fun clearCredentialsIfNotRemember() {
        if (!remember) {
            kv.removeValuesForKeys(arrayOf("tel", "pass"))
        }
    }
}
