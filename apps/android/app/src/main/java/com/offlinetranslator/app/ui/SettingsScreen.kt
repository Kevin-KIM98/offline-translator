package com.offlinetranslator.app.ui

import android.app.ActivityManager
import android.content.Context
import androidx.activity.compose.BackHandler
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
import androidx.compose.material3.RadioButton
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.offlinetranslator.ModelState
import com.offlinetranslator.TranslationBackend
import com.offlinetranslator.app.Lang
import com.offlinetranslator.app.MainViewModel
import com.offlinetranslator.app.R
import com.offlinetranslator.app.Side
import com.offlinetranslator.app.UiState
import com.offlinetranslator.app.mb
import com.offlinetranslator.app.modelTitle
import com.offlinetranslator.app.ui.theme.LocalSpeakerColors

@Composable
fun SettingsScreen(vm: MainViewModel, state: UiState, onBack: () -> Unit) {
    var picking by remember { mutableStateOf<Side?>(null) }
    val speakers = LocalSpeakerColors.current
    val context = LocalContext.current

    BackHandler(onBack = onBack)

    LaunchedEffect(Unit) { vm.refreshInstalled() }
    val engineInfo = remember(state.phase) { vm.engineInfo() }

    Column(Modifier.fillMaxSize().statusBarsPadding()) {
        Row(
            Modifier.fillMaxWidth().padding(start = 4.dp, end = 16.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = onBack) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back), tint = MaterialTheme.colorScheme.onSurface)
            }
            Text(stringResource(R.string.settings), style = MaterialTheme.typography.titleMedium)
        }

        Column(
            Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp)
                .navigationBarsPadding(),
        ) {
            SectionLabel(stringResource(R.string.settings_languages))
            Card {
                SettingRow(stringResource(R.string.settings_speaker_left), Lang.of(state.langA).name, speakers.sideA) { picking = Side.A }
                Divider()
                SettingRow(stringResource(R.string.settings_speaker_right), Lang.of(state.langB).name, speakers.sideB) { picking = Side.B }
            }

            Spacer(Modifier.height(20.dp))
            SectionLabel(stringResource(R.string.settings_sound))
            Card {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(R.string.settings_speak), style = MaterialTheme.typography.bodyLarge)
                        Text(
                            stringResource(R.string.settings_speak_sub),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Switch(checked = state.speak, onCheckedChange = vm::setSpeak)
                }
                Divider()
                Column(Modifier.padding(16.dp)) {
                    Row {
                        Text(stringResource(R.string.settings_rate), style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
                        Text(
                            stringResource(R.string.settings_rate_value, state.speechRate),
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
            SectionLabel(stringResource(R.string.settings_backend))
            Card {
                Column(Modifier.padding(16.dp)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        BackendChip(stringResource(R.string.backend_auto), TranslationBackend.AUTO, state.backend, vm::setBackend)
                        BackendChip(stringResource(R.string.backend_marian), TranslationBackend.MARIAN, state.backend, vm::setBackend)
                        BackendChip(stringResource(R.string.backend_llm), TranslationBackend.LLM, state.backend, vm::setBackend)
                    }
                    Spacer(Modifier.height(10.dp))
                    Text(
                        when (state.backend) {
                            TranslationBackend.AUTO -> stringResource(R.string.backend_auto_desc)
                            TranslationBackend.MARIAN -> stringResource(R.string.backend_marian_desc)
                            TranslationBackend.LLM -> stringResource(R.string.backend_llm_desc)
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            if (state.sttOptions.size > 1) {
                Spacer(Modifier.height(20.dp))
                SectionLabel(stringResource(R.string.settings_stt))
                Card {
                    val selectedId = state.sttId ?: state.sttOptions.first().id
                    state.sttOptions.forEachIndexed { i, m ->
                        val choice = if (i == 0) null else m.id   // null follows the manifest's default
                        if (i > 0) Divider()
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clickable { vm.setStt(choice) }
                                .padding(start = 16.dp, end = 4.dp, top = 10.dp, bottom = 10.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Column(Modifier.weight(1f)) {
                                Text(m.label.ifBlank { m.id }, style = MaterialTheme.typography.bodyLarge)
                                Text(
                                    mb(m.totalBytes) + " · " + stringResource(
                                        if (m.state == ModelState.READY) R.string.llm_installed else R.string.llm_not_installed
                                    ),
                                    style = MaterialTheme.typography.labelMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            RadioButton(selected = m.id == selectedId, onClick = { vm.setStt(choice) })
                        }
                    }
                    Divider()
                    Text(
                        stringResource(R.string.settings_stt_desc),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(16.dp),
                    )
                }
            }

            if (state.llmOptions.size > 1) {
                Spacer(Modifier.height(20.dp))
                SectionLabel(stringResource(R.string.settings_llm))
                Card {
                    val selectedId = state.llmId ?: state.llmOptions.first().id
                    state.llmOptions.forEachIndexed { i, m ->
                        // The first entry is the manifest's default; storing null for it keeps the
                        // choice following the manifest if the default ever changes.
                        val choice = if (i == 0) null else m.id
                        if (i > 0) Divider()
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clickable { vm.setLlm(choice) }
                                .padding(start = 16.dp, end = 4.dp, top = 10.dp, bottom = 10.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Column(Modifier.weight(1f)) {
                                Text(m.label.ifBlank { m.id }, style = MaterialTheme.typography.bodyLarge)
                                Text(
                                    mb(m.totalBytes) + " · " + stringResource(
                                        if (m.state == ModelState.READY) R.string.llm_installed else R.string.llm_not_installed
                                    ),
                                    style = MaterialTheme.typography.labelMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            RadioButton(selected = m.id == selectedId, onClick = { vm.setLlm(choice) })
                        }
                    }
                    Divider()
                    Text(
                        stringResource(R.string.settings_llm_desc),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(16.dp),
                    )
                    val totalMem = remember { totalMemoryBytes(context) }
                    if (totalMem in 1 until 6_000_000_000L) {
                        Text(
                            stringResource(R.string.settings_llm_memory_warning, String.format("%.1f GB", totalMem / 1e9)),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.error,
                            modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp),
                        )
                    }
                }
            }

            Spacer(Modifier.height(20.dp))
            SectionLabel(stringResource(R.string.settings_installed))
            Card {
                if (state.installed.isEmpty()) {
                    Text(
                        stringResource(R.string.settings_none_installed),
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
                                Text(modelTitle(context, m), style = MaterialTheme.typography.bodyMedium)
                                Text(
                                    mb(m.totalBytes),
                                    style = MaterialTheme.typography.labelMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            IconButton(onClick = { vm.removeModel(m.id) }) {
                                Icon(
                                    Icons.Filled.DeleteOutline,
                                    stringResource(R.string.delete),
                                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                    modifier = Modifier.size(20.dp),
                                )
                            }
                        }
                    }
                    if (ready.isNotEmpty()) {
                        Divider()
                        Text(
                            stringResource(R.string.settings_total, mb(ready.sumOf { it.totalBytes })),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(16.dp),
                        )
                    }
                }
            }

            Spacer(Modifier.height(20.dp))
            SectionLabel(stringResource(R.string.settings_about))
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
                stringResource(R.string.settings_privacy),
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
            title = stringResource(if (side == Side.A) R.string.picker_left else R.string.picker_right),
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

private fun totalMemoryBytes(context: Context): Long {
    val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return 0L
    val info = ActivityManager.MemoryInfo()
    am.getMemoryInfo(info)
    return info.totalMem
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
