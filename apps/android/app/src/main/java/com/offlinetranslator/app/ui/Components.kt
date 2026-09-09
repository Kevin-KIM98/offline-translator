package com.offlinetranslator.app.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.offlinetranslator.app.Lang

/**
 * Press-and-hold microphone button. Holding is deliberate: it makes the utterance boundary
 * explicit, which is both faster and more reliable than waiting for silence detection.
 */
@Composable
fun TalkButton(
    label: String,
    accent: Color,
    active: Boolean,
    level: Float,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onPress: () -> Unit,
    onRelease: () -> Unit,
) {
    val bg by animateColorAsState(
        when {
            !enabled -> MaterialTheme.colorScheme.surfaceVariant
            active -> accent
            else -> accent.copy(alpha = 0.16f)
        },
        tween(120), label = "talkBg",
    )
    val glow by animateFloatAsState(if (active) level.coerceIn(0f, 1f) else 0f, tween(90), label = "talkGlow")
    val content = when {
        !enabled -> MaterialTheme.colorScheme.onSurfaceVariant
        active -> Color.White
        else -> accent
    }

    Box(
        modifier = modifier
            .height(104.dp)
            .clip(RoundedCornerShape(22.dp))
            .background(bg)
            .border(
                width = (1 + 3 * glow).dp,
                color = if (active) Color.White.copy(alpha = 0.25f + 0.45f * glow) else accent.copy(alpha = 0.35f),
                shape = RoundedCornerShape(22.dp),
            )
            .pointerInput(enabled) {
                if (!enabled) return@pointerInput
                detectTapGestures(onPress = {
                    onPress()
                    tryAwaitRelease()
                    onRelease()
                })
            },
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(Icons.Filled.Mic, contentDescription = null, tint = content, modifier = Modifier.size(26.dp))
            Spacer(Modifier.height(6.dp))
            Text(
                label,
                style = MaterialTheme.typography.labelLarge,
                color = content,
                textAlign = TextAlign.Center,
                maxLines = 1,
            )
            Text(
                if (active) "말하는 중" else "길게 눌러 말하기",
                style = MaterialTheme.typography.labelMedium,
                color = content.copy(alpha = 0.75f),
                maxLines = 1,
            )
        }
    }
}

/** Level meter drawn as a row of bars — cheap, and readable in bright light. */
@Composable
fun LevelBars(level: Float, accent: Color, bars: Int = 12, modifier: Modifier = Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(3.dp), verticalAlignment = Alignment.CenterVertically) {
        repeat(bars) { i ->
            val threshold = (i + 1f) / bars
            val on = level >= threshold
            Box(
                Modifier
                    .width(4.dp)
                    .height(if (on) (8 + 14 * threshold).dp else 6.dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(if (on) accent else accent.copy(alpha = 0.20f))
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LanguagePickerSheet(
    title: String,
    current: String,
    disabled: String?,
    onPick: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val sheet = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheet) {
        Column(Modifier.padding(bottom = 12.dp).navigationBarsPadding()) {
            Text(
                title,
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(start = 24.dp, end = 24.dp, bottom = 8.dp),
            )
            Lang.ALL.forEach { lang ->
                val isDisabled = lang.code == disabled
                Row(
                    Modifier
                        .fillMaxWidth()
                        .pointerInput(lang.code, isDisabled) {
                            detectTapGestures { if (!isDisabled) onPick(lang.code) }
                        }
                        .padding(horizontal = 24.dp, vertical = 14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        lang.name,
                        style = MaterialTheme.typography.bodyLarge,
                        color = when {
                            isDisabled -> MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
                            lang.code == current -> MaterialTheme.colorScheme.primary
                            else -> MaterialTheme.colorScheme.onSurface
                        },
                        modifier = Modifier.weight(1f),
                    )
                    if (lang.code == current) {
                        Icon(Icons.Filled.Check, null, tint = MaterialTheme.colorScheme.primary)
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
        }
    }
}

/** Simple section header used by the settings and setup screens. */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text.uppercase(),
        style = MaterialTheme.typography.labelMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = modifier.padding(start = 4.dp, bottom = 8.dp, top = 4.dp),
    )
}
