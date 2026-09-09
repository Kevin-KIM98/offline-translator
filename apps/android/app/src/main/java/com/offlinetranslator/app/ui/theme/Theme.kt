package com.offlinetranslator.app.ui.theme

import android.app.Activity
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat

// The two speakers get a warm/cool pair so a glance tells you whose line you are reading.
// Everything else stays near-neutral: the translated sentence is the only loud thing on screen.

private val TealDark = Color(0xFF2CAE9E)
private val TealLight = Color(0xFF0E6E63)
private val AmberDark = Color(0xFFE0A33E)
private val AmberLight = Color(0xFF8A5A0B)

private val DarkScheme = darkColorScheme(
    primary = TealDark,
    onPrimary = Color(0xFF00201C),
    primaryContainer = Color(0xFF14413B),
    onPrimaryContainer = Color(0xFFA8EFE4),
    secondary = AmberDark,
    onSecondary = Color(0xFF241704),
    background = Color(0xFF0E1513),
    onBackground = Color(0xFFF2F5F4),
    surface = Color(0xFF161E1C),
    onSurface = Color(0xFFF2F5F4),
    surfaceVariant = Color(0xFF1F2A27),
    onSurfaceVariant = Color(0xFF9EB0AC),
    outline = Color(0xFF3A4A46),
    error = Color(0xFFE5705F),
    onError = Color(0xFF2B0906),
)

private val LightScheme = lightColorScheme(
    primary = TealLight,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFCFEDE7),
    onPrimaryContainer = Color(0xFF00201C),
    secondary = AmberLight,
    onSecondary = Color.White,
    background = Color(0xFFF6F7F5),
    onBackground = Color(0xFF101615),
    surface = Color(0xFFFFFFFF),
    onSurface = Color(0xFF101615),
    surfaceVariant = Color(0xFFE7EDEB),
    onSurfaceVariant = Color(0xFF55645F),
    outline = Color(0xFFBFCBC7),
    error = Color(0xFFB3261E),
    onError = Color.White,
)

/** Per-speaker accents, kept outside the Material scheme so both stay first-class. */
data class SpeakerColors(val sideA: Color, val sideB: Color, val onSide: Color)

val LocalSpeakerColors = staticCompositionLocalOf {
    SpeakerColors(TealDark, AmberDark, Color.White)
}

private val AppTypography = Typography(
    // The translated sentence: the reason the app exists.
    headlineSmall = TextStyle(fontSize = 24.sp, lineHeight = 32.sp, fontWeight = FontWeight.SemiBold),
    titleMedium = TextStyle(fontSize = 17.sp, lineHeight = 24.sp, fontWeight = FontWeight.SemiBold),
    bodyLarge = TextStyle(fontSize = 16.sp, lineHeight = 24.sp),
    bodyMedium = TextStyle(fontSize = 14.sp, lineHeight = 20.sp),
    bodySmall = TextStyle(fontSize = 13.sp, lineHeight = 18.sp),
    labelLarge = TextStyle(fontSize = 15.sp, lineHeight = 20.sp, fontWeight = FontWeight.SemiBold),
    labelMedium = TextStyle(fontSize = 12.sp, lineHeight = 16.sp, fontWeight = FontWeight.Medium, letterSpacing = 0.4.sp),
)

@Composable
fun InterpreterTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    val scheme = if (dark) DarkScheme else LightScheme
    val speakers = SpeakerColors(
        sideA = if (dark) TealDark else TealLight,
        sideB = if (dark) AmberDark else AmberLight,
        onSide = Color.White,
    )
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = scheme.background.toArgb()
            window.navigationBarColor = scheme.background.toArgb()
            WindowCompat.getInsetsController(window, view).apply {
                isAppearanceLightStatusBars = !dark
                isAppearanceLightNavigationBars = !dark
            }
        }
    }
    CompositionLocalProvider(LocalSpeakerColors provides speakers) {
        MaterialTheme(colorScheme = scheme, typography = AppTypography, content = content)
    }
}
