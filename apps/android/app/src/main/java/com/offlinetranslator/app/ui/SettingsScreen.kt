package com.offlinetranslator.app.ui

import androidx.compose.foundation.clickable
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
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.DeleteOutline
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.offlinetranslator.ModelState
import com.offlinetranslator.TranslationBackend
import com.offlinetranslator.app.Lang
import com.offlinetranslator.app.MainViewModel
import com.offlinetranslator.app.Side
import com.offlinetranslator.app.UiState
import com.offlinetranslator.app.mb
import com.offlinetranslator.app.ui.theme.LocalSpeakerColors

@Composable
fun SettingsScreen(vm: MainViewModel, state: UiState, onBack: () -> Unit) {
    var picking by remember { mutableStateOf<Side?>(null) }
    val speakers = LocalSpeakerColors.current

    LaunchedEffect(Unit) { vm.refreshInstalled() }
    val engineInfo = remember(state.phase) { vm.engineInfo() }

    Column(Modifier.fillMaxSize().statusBarsPadding()) {
        Row(
            Modifier.fillMaxWidth().padding(start = 4.dp, end = 16.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = onBack) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, "뒤로", tint = MaterialTheme.colorScheme.onSurface)
            }
            Text("설정", style = MaterialTheme.typography.titleMedium)
        }

        Column(
            Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp)
                .navigationBarsPadding(),
        ) {
            SectionLabel("언어")
            Card {
                SettingRow("왼쪽 화자", Lang.of(state.langA).name, speakers.sideA) { picking = Side.A }
                Divider()
                SettingRow("오른쪽 화자", Lang.of(state.langB).name, speakers.sideB) { picking = Side.B }
            }

            Spacer(Modifier.height(20.dp))
            SectionLabel("소리")
            Card {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("번역 읽어주기", style = MaterialTheme.typography.bodyLarge)
                        Text(
                            "기기에 설치된 음성으로 재생합니다",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Switch(checked = state.speak, onCheckedChange = vm::setSpeak)
                }
                Divider()
                Column(Modifier.padding(16.dp)) {
                    Row {
                        Text("말하기 속도", style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
                        Text(
                            String.format("%.1f×", state.speechRate),
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Slider(
                        value = state.speechRate,
                        onValueChange = vm::setSpeechRate,
                        valueRange = 0.6f..1.6f,
                        steps = 4,
                    )
                }
            }

            Spacer(Modifier.height(20.dp))
            SectionLabel("번역 엔진")
            Card {
                Column(Modifier.padding(16.dp)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        BackendChip("자동", TranslationBackend.AUTO, state.backend, vm::setBackend)
                        BackendChip("전용 모델", TranslationBackend.MARIAN, state.backend, vm::setBackend)
                        BackendChip("LLM", TranslationBackend.LLM, state.backend, vm::setBackend)
                    }
                    Spacer(Modifier.height(10.dp))
                    Text(
                        when (state.backend) {
                            TranslationBackend.AUTO ->
                                "언어쌍 전용 모델이 있으면 그것을 쓰고, 없으면 LLM으로 번역합니다. 가장 빠릅니다."
                            TranslationBackend.MARIAN ->
                                "전용 번역 모델만 사용합니다. 태국어처럼 모델이 없는 방향은 번역되지 않습니다."
                            TranslationBackend.LLM ->
                                "모든 방향을 LLM 하나로 번역합니다. 느리지만 언어를 자동으로 알아냅니다. 1.1 GB가 필요합니다."
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            Spacer(Modifier.height(20.dp))
            SectionLabel("설치된 모델")
            Card {
                if (state.installed.isEmpty()) {
                    Text(
                        "아직 설치된 모델이 없습니다.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(16.dp),
                    )
                } else {
                    val ready = state.installed.filter { it.state == ModelState.READY }
                    ready.forEachIndexed { i, m ->
                        if (i > 0) Divider()
                        Row(Modifier.padding(start = 16.dp, end = 6.dp, top = 12.dp, bottom = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(modelTitle(m), style = MaterialTheme.typography.bodyMedium)
                                Text(
                                    mb(m.totalBytes),
                                    style = MaterialTheme.typography.labelMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            IconButton(onClick = { vm.removeModel(m.id) }) {
                                Icon(
                                    Icons.Filled.DeleteOutline,
                                    "삭제",
                                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                    modifier = Modifier.size(20.dp),
                                )
                            }
                        }
                    }
                    if (ready.isNotEmpty()) {
                        Divider()
                        Text(
                            "합계 ${mb(ready.sumOf { it.totalBytes })}",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(16.dp),
                        )
                    }
                }
            }

            Spacer(Modifier.height(20.dp))
            SectionLabel("정보")
            Card {
                engineInfo.forEachIndexed { i, (k, v) ->
                    if (i > 0) Divider()
                    Row(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
                        Text(k, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                        Text(
                            v,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }

            Spacer(Modifier.height(16.dp))
            Text(
                "모든 처리는 기기 안에서 이루어집니다. 음성과 문장은 어디로도 전송되지 않습니다.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.outline,
            )
            Spacer(Modifier.height(32.dp))
        }
    }

    picking?.let { side ->
        val current = if (side == Side.A) state.langA else state.langB
        val other = if (side == Side.A) state.langB else state.langA
        LanguagePickerSheet(
            title = if (side == Side.A) "왼쪽 화자의 언어" else "오른쪽 화자의 언어",
            current = current,
            disabled = other,
            onPick = { code ->
                if (side == Side.A) vm.setLanguages(code, state.langB) else vm.setLanguages(state.langA, code)
                picking = null
            },
            onDismiss = { picking = null },
        )
    }
}

@Composable
private fun BackendChip(
    label: String,
    value: TranslationBackend,
    current: TranslationBackend,
    onPick: (TranslationBackend) -> Unit,
) {
    FilterChip(selected = current == value, onClick = { onPick(value) }, label = { Text(label) })
}

@Composable
private fun Card(content: @Composable () -> Unit) {
    Surface(
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column { content() }
    }
}

@Composable
private fun Divider() {
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        modifier = Modifier.fillMaxWidth().height(1.dp),
    ) {}
}

@Composable
private fun SettingRow(title: String, value: String, accent: androidx.compose.ui.graphics.Color, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(title, style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
        Text(value, style = MaterialTheme.typography.labelLarge, color = accent)
        Spacer(Modifier.width(4.dp))
    }
}
