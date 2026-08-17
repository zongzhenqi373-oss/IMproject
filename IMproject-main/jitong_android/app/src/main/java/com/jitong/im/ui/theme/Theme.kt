package com.jitong.im.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val JitongColorScheme = lightColorScheme(
    primary = Color(0xFF07C160),      // 微信绿
    onPrimary = Color.White,
    secondary = Color(0xFF576B95),
    surface = Color(0xFFF7F7F7),
    background = Color.White,
)

@Composable
fun JitongTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = JitongColorScheme,
        content = content,
    )
}
