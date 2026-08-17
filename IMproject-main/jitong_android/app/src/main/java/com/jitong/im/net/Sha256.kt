package com.jitong.im.net

import java.security.MessageDigest

/**
 * SHA-256 小写十六进制，与 C++ 端 im::sha256Hex（protocol/sha256.h）输出一致。
 * 对齐 QQNT：密码绝不原文上链路，客户端先哈希一次，服务端再加盐二次哈希存库。
 */
fun sha256Hex(input: String): String {
    val digest = MessageDigest.getInstance("SHA-256").digest(input.toByteArray(Charsets.UTF_8))
    return digest.joinToString("") { "%02x".format(it) }
}
