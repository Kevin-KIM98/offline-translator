package com.offlinetranslator.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CloudDownload
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.offlinetranslator.ModelStatus
import com.offlinetranslator.app.Lang
import com.offlinetranslator.app.Phase
import com.offlinetranslator.app.UiState
import com.offlinetranslator.app.mb

@Composable
fun SetupScreen(
    state: UiState,
    phase: Phase.Setup,
    onDownload: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    val total = phase.pending.sumOf { it.totalBytes }
    Column(
        Modifier
            .fillMaxSize()
            .statusBarsPadding()
            .navigationBarsPadding()
            .padding(24.dp)
    ) {
        Spacer(Modifier.height(16.dp))
        Text(
            "통역을 시작하기 전에",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onBackground,
        )
        Spacer(Modifier.height(8.dp))
        Text(
            "${Lang.of(state.langA).name}와 ${Lang.of(state.langB).name}를 통역하려면 아래 모델을 한 번만 내려받으면 됩니다. " +
                "그 뒤로는 인터넷 없이 동작합니다.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Spacer(Modifier.height(24.dp))
        Column(
            Modifier.weight(1f).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            phase.pending.forEach { ModelRow(it) }
            if (phase.pending.any { it.kind == "llm" }) {
                Spacer(Modifier.height(4.dp))
                Note(
                    "선택한 언어 조합에는 직접 번역 모델이 없어서 다국어 LLM이 함께 설치됩니다. " +
                        "설정에서 다른 언어를 고르면 용량을 줄일 수 있습니다."
                )
            }
        }

        phase.error?.let {
            Spacer(Modifier.height(12.dp))
            Surface(
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.error.copy(alpha = 0.12f),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Filled.ErrorOutline, null, tint = MaterialTheme.colorScheme.error, modifier = Modifier.size(20.dp))
                    Spacer(Modifier.width(10.dp))
                    Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                }
            }
        }

        Spacer(Modifier.height(16.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Filled.Wifi, null, tint = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
            Text(
                "Wi-Fi에서 받는 것을 권합니다. 중단해도 이어받습니다.",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.height(12.dp))
        Button(onClick = onDownload, modifier = Modifier.fillMaxWidth().height(54.dp)) {
            Icon(Icons.Filled.CloudDownload, null, Modifier.size(20.dp))
            Spacer(Modifier.width(10.dp))
            Text("${mb(total)} 내려받기", style = MaterialTheme.typography.labelLarge)
        }
        TextButton(onClick = onOpenSettings, modifier = Modifier.fillMaxWidth()) {
            Text("언어 바꾸기")
        }
    }
}

@Composable
private fun ModelRow(m: ModelStatus) {
    Surface(
        shape = RoundedCornerShape(14.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(modelTitle(m), style = MaterialTheme.typography.bodyLarge)
                Spacer(Modifier.height(2.dp))
                Text(
                    modelSubtitle(m),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Text(
                mb(m.totalBytes),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun Note(text: String) {
    Surface(
        shape = RoundedCornerShape(14.dp),
        color = MaterialTheme.colorScheme.surfaceVariant,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Text(
            text,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(14.dp),
        )
    }
}

@Composable
fun DownloadScreen(phase: Phase.Downloading, onCancel: () -> Unit) {
    val fraction = if (phase.bytesTotal > 0) (phase.bytesDone.toFloat() / phase.bytesTotal).coerceIn(0f, 1f) else 0f
    Column(
        Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(32.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            if (phase.verifying) "파일을 검사하는 중" else "모델을 내려받는 중",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onBackground,
        )
        Spacer(Modifier.height(6.dp))
        Text(phase.label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(20.dp))
        LinearProgressIndicator(
            progress = { fraction },
            modifier = Modifier.fillMaxWidth().height(8.dp),
            strokeCap = androidx.compose.ui.graphics.StrokeCap.Round,
        )
        Spacer(Modifier.height(10.dp))
        Text(
            "${mb(phase.bytesDone)} / ${mb(phase.bytesTotal)}  ·  ${(fraction * 100).toInt()}%",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(28.dp))
        Text(
            "이 화면을 켜 둔 채로 기다려 주세요.\n앱을 닫으면 받은 지점부터 다시 이어집니다.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.outline,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(12.dp))
        TextButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) { Text("멈추기") }
    }
}

internal fun modelTitle(m: ModelStatus): String = when (m.kind) {
    "stt" -> "음성 인식"
    "llm" -> "다국어 통역 LLM"
    "nmt" -> {
        val parts = m.pair.split("-")
        if (parts.size == 2) "${Lang.of(parts[0]).name} → ${Lang.of(parts[1]).name} 번역"
        else "번역 모델 ${m.pair}"
    }
    else -> m.id
}

internal fun modelSubtitle(m: ModelStatus): String = when (m.kind) {
    "stt" -> "whisper · 7개 언어 공용"
    "llm" -> "Qwen2.5 · 모든 언어 조합"
    "nmt" -> "OPUS-MT"
    else -> m.kind
}
