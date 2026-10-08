package com.example.cdplayer.ui.theme

import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val DarkColorScheme = darkColorScheme(
    primary = Color(0xFF6366F1), // Indigo Neon
    onPrimary = Color.White,
    primaryContainer = Color(0xFF3730A3),
    onPrimaryContainer = Color(0xFFE0E7FF),
    secondary = Color(0xFF10B981), // Emerald Accent
    onSecondary = Color.Black,
    background = Color(0xFF0D0D11), // Hi-Fi Dark Pitch
    onBackground = Color(0xFFF3F4F6),
    surface = Color(0xFF16161E), // Glass Card Surface
    onSurface = Color(0xFFE5E7EB),
    surfaceVariant = Color(0xFF1F1F2E),
    onSurfaceVariant = Color(0xFF9CA3AF),
    error = Color(0xFFEF4444)
)

@Composable
fun RpiCdPlayerTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DarkColorScheme,
        content = content
    )
}
