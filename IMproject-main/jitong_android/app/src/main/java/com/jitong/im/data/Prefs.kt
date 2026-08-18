package com.jitong.im.data

import com.tencent.mmkv.MMKV

/**
 * KV 凭证存储（MMKV，对齐 QQNT 双存储：MMKV 管 KV/凭证，Room 管消息）。
 * 存的是手机号 + 密码哈希（不是明文），用于自动登录；下线即清除。
 */
object Prefs {
    private val kv: MMKV by lazy { MMKV.defaultMMKV() }

    var tel: String?
        get() = kv.decodeString("tel", null)?.takeIf { it.isNotEmpty() }
        set(v) {
            if (v == null) kv.removeValueForKey("tel") else kv.encode("tel", v)
        }

    var passHash: String?
        get() = kv.decodeString("passHash", null)?.takeIf { it.isNotEmpty() }
        set(v) {
            if (v == null) kv.removeValueForKey("passHash") else kv.encode("passHash", v)
        }

    fun clearCredentials() {
        kv.removeValuesForKeys(arrayOf("tel", "passHash"))
    }
}
