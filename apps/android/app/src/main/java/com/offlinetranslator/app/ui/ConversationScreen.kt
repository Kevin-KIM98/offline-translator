package com.offlinetranslator.app.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Keyboard
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.StopCircle
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.offlinetranslator.app.Lang
import com.offlinetranslator.app.MainViewModel
import com.offlinetranslator.app.Side
import com.offlinetranslator.app.Turn
import com.offlinetranslator.app.UiState
import com.offlinetranslator.app.ui.theme.LocalSpeakerColors

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConversationScreen(
    vm: MainViewModel,
    state: UiState,
    micGranted: Boolean,
    onRequestMic: () -> Unit,
    onOpenAppSettings: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    val speakers = LocalSpeakerColors.current
    val snackbar = remember { SnackbarHostState() }
    var picking by remember { mutableStateOf<Side?>(null) }
    var typing by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()

    LaunchedEffect(state.message) {
        state.message?.let {
            snackbar.showSnackbar(it)
            vm.dismissMessage()
        }
    }
    LaunchedEffect(state.turns.size) {
        if (state.turns.isNotEmpty()) listState.animateScrollToItem(state.turns.lastIndex)
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbar) },
        containerColor = MaterialTheme.colorScheme.background,
        // topBar/bottomBar apply the system bar insets themselves.
        contentWindowInsets = WindowInsets(0, 0, 0, 0),
        topBar = {
            LanguageBar(
                state = state,
                onPickA = { picking = Side.A },
                onPickB = { picking = Side.B },
                onSwap = vm::swapLanguages,
                onSettings = onOpenSettings,
            )
        },
        bottomBar = {
            Column(
                Modifier
                    .background(MaterialTheme.colorScheme.surface)
                    .navigationBarsPadding()
                    .padding(horizontal = 16.dp, vertical = 12.dp)
            ) {
                ActionRow(
                    state = state,
                    onTyping = { typing = true },
                    onClear = vm::clearConversation,
                    onHandsFree = { vm.setHandsFree(!state.handsFree) },
                )
                Spacer(Modifier.height(10.dp))
                when {
                    !micGranted -> MicPermissionCard(onRequestMic, onOpenAppSettings)
                    state.handsFree -> HandsFreePanel(state, onStop = { vm.setHandsFree(false) })
                    else -> Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        TalkButton(
                            label = Lang.of(state.langA).short,
                            accent = speakers.sideA,
                            active = state.listening == Side.A,
                            level = state.level,
                            enabled = state.listening != Side.B,
                            modifier = Modifier.weight(1f),
                            onPress = { vm.startTalking(Side.A) },
                            onRelease = vm::stopTalking,
                        )
                        TalkButton(
                            label = Lang.of(state.langB).short,
                            accent = speakers.sideB,
                            active = state.listening == Side.B,
                            level = state.level,
                            enabled = state.listening != Side.A,
                            modifier = Modifier.weight(1f),
                            onPress = { vm.startTalking(Side.B) },
                            onRelease = vm::stopTalking,
                        )
                    }
                }
            }
        },
    ) { inner ->
        Box(Modifier.fillMaxSize().padding(inner)) {
            if (state.turns.isEmpty()) {
                EmptyState(state)
            } else {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    items(state.turns, key = { it.id }) { turn ->
                        TurnCard(
                            turn = turn,
                            speaking = state.speakingTurn == turn.id,
                            onReplay = { if (state.speakingTurn == turn.id) vm.stopSpeaking() else vm.speakTurn(turn) },
                        )
                    }
                }
            }
            AnimatedVisibility(
                visible = state.working,
                modifier = Modifier.align(Alignment.BottomCenter),
            ) {
                WorkingBar()
            }
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

    if (typing) {
        TextInputSheet(state = state, onDismiss = { typing = false }, onSubmit = { text, side ->
            vm.translateText(text, side) {}
            typing = false
        })
    }
}

@Composable
private fun LanguageBar(
    state: UiState,
    onPickA: () -> Unit,
    onPickB: () -> Unit,
    onSwap: () -> Unit,
    onSettings: () -> Unit,
) {
    val speakers = LocalSpeakerColors.current
    Surface(color = MaterialTheme.colorScheme.surface) {
        Row(
            Modifier
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(start = 12.dp, end = 4.dp, top = 8.dp, bottom = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            LanguageChip(Lang.of(state.langA).name, speakers.sideA, Modifier.weight(1f), onPickA)
            IconButton(onClick = onSwap) {
                Icon(Icons.Filled.SwapHoriz, "언어 방향 바꾸기", tint = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            LanguageChip(Lang.of(state.langB).name, speakers.sideB, Modifier.weight(1f), onPickB)
            IconButton(onClick = onSettings) {
                Icon(Icons.Filled.Settings, "설정", tint = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun LanguageChip(name: String, accent: Color, modifier: Modifier, onClick: () -> Unit) {
    Surface(
        modifier = modifier.clickable(onClick = onClick),
        shape = RoundedCornerShape(12.dp),
        color = accent.copy(alpha = 0.14f),
    ) {
        Text(
            name,
            style = MaterialTheme.typography.labelLarge,
            color = accent,
            textAlign = TextAlign.Center,
            maxLines = 1,
            modifier = Modifier.fillMaxWidth().padding(vertical = 10.dp, horizontal = 8.dp),
        )
    }
}

@Composable
private fun ActionRow(state: UiState, onTyping: () -> Unit, onClear: () -> Unit, onHandsFree: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        FilterChip(
            selected = state.handsFree,
            onClick = onHandsFree,
            label = { Text("핸즈프리") },
            leadingIcon = { Icon(Icons.Filled.GraphicEq, null, Modifier.size(18.dp)) },
            colors = FilterChipDefaults.filterChipColors(),
        )
        Spacer(Modifier.weight(1f))
        IconButton(onClick = onTyping) {
            Icon(Icons.Filled.Keyboard, "키보드로 입력", tint = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        IconButton(onClick = onClear, enabled = state.turns.isNotEmpty()) {
            Icon(
                Icons.Filled.DeleteSweep,
                "대화 지우기",
                tint = if (state.turns.isEmpty()) MaterialTheme.colorScheme.outline
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun HandsFreePanel(state: UiState, onStop: () -> Unit) {
    val speakers = LocalSpeakerColors.current
    Surface(
        shape = RoundedCornerShape(22.dp),
        color = speakers.sideA.copy(alpha = 0.12f),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(18.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    "핸즈프리로 듣는 중",
                    style = MaterialTheme.typography.labelLarge,
                    color = speakers.sideA,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    "말이 끝나면 자동으로 통역합니다. 두 사람이 번갈아 말해도 됩니다.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(10.dp))
                LevelBars(state.level, speakers.sideA)
            }
            Spacer(Modifier.width(12.dp))
            IconButton(onClick = onStop) {
                Icon(Icons.Filled.StopCircle, "중지", tint = speakers.sideA, modifier = Modifier.size(34.dp))
            }
        }
    }
}

@Composable
private fun MicPermissionCard(onRequest: () -> Unit, onOpenSettings: () -> Unit) {
    Surface(
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(18.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.MicOff, null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.width(10.dp))
                Text("마이크 권한이 필요합니다", style = MaterialTheme.typography.labelLarge)
            }
            Spacer(Modifier.height(6.dp))
            Text(
                "음성은 기기 안에서만 처리되며 어디로도 전송되지 않습니다.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(12.dp))
            Row {
                Button(onClick = onRequest) { Text("권한 허용") }
                Spacer(Modifier.width(8.dp))
                TextButton(onClick = onOpenSettings) { Text("설정에서 변경") }
            }
        }
    }
}

@Composable
private fun WorkingBar() {
    Column(Modifier.fillMaxWidth()) {
        Text(
            "통역하는 중",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(start = 20.dp, bottom = 4.dp),
        )
        LinearProgressIndicator(Modifier.fillMaxWidth().height(2.dp))
    }
}

@Composable
private fun EmptyState(state: UiState) {
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            "${Lang.of(state.langA).name} ↔ ${Lang.of(state.langB).name}",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onBackground,
        )
        Spacer(Modifier.height(10.dp))
        Text(
            "아래 버튼을 길게 누른 채 말하고, 말이 끝나면 손을 떼세요.\n" +
                "번역이 화면에 뜨고 소리로도 나옵니다.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(18.dp))
        Text(
            "인터넷 없이 동작합니다.",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.primary,
        )
    }
}

@Composable
private fun TurnCard(turn: Turn, speaking: Boolean, onReplay: () -> Unit) {
    val speakers = LocalSpeakerColors.current
    val accent = if (turn.side == Side.A) speakers.sideA else speakers.sideB
    Row(Modifier.fillMaxWidth()) {
        if (turn.side == Side.B) Spacer(Modifier.width(28.dp))
        Surface(
            shape = RoundedCornerShape(
                topStart = if (turn.side == Side.A) 6.dp else 18.dp,
                topEnd = if (turn.side == Side.A) 18.dp else 6.dp,
                bottomStart = 18.dp,
                bottomEnd = 18.dp,
            ),
            color = MaterialTheme.colorScheme.surface,
            modifier = Modifier.weight(1f),
        ) {
            Row(Modifier.height(IntrinsicSize.Min)) {
                Box(
                    Modifier
                        .width(4.dp)
                        .fillMaxHeight()
                        .background(accent)
                )
                Column(Modifier.padding(14.dp)) {
                    Text(
                        turn.sourceText,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        turn.translatedText.ifBlank { "(번역 없음)" },
                        style = MaterialTheme.typography.headlineSmall,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Spacer(Modifier.height(10.dp))
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            "${Lang.of(turn.sourceLang).name} → ${Lang.of(turn.targetLang).name} · " +
                                "${turn.totalMs.toInt()} ms" +
                                if (turn.route.isEmpty()) "" else " · ${turn.route.joinToString("→")}",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.outline,
                            modifier = Modifier.weight(1f),
                        )
                        Box(
                            Modifier
                                .size(34.dp)
                                .clip(CircleShape)
                                .clickable(onClick = onReplay),
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(
                                if (speaking) Icons.Filled.StopCircle else Icons.Filled.VolumeUp,
                                contentDescription = if (speaking) "멈추기" else "다시 듣기",
                                tint = if (speaking) accent else MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.size(20.dp),
                            )
                        }
                    }
                }
            }
        }
        if (turn.side == Side.A) Spacer(Modifier.width(28.dp))
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TextInputSheet(state: UiState, onDismiss: () -> Unit, onSubmit: (String, Side) -> Unit) {
    val sheet = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    var text by remember { mutableStateOf("") }
    var side by remember { mutableStateOf(Side.A) }
    val speakers = LocalSpeakerColors.current

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheet) {
        Column(Modifier.padding(horizontal = 20.dp).padding(bottom = 20.dp).imePadding().navigationBarsPadding()) {
            Text("키보드로 통역", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(12.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(Side.A to state.langA, Side.B to state.langB).forEach { (s, code) ->
                    FilterChip(
                        selected = side == s,
                        onClick = { side = s },
                        label = { Text(Lang.of(code).name) },
                        colors = FilterChipDefaults.filterChipColors(
                            selectedContainerColor = (if (s == Side.A) speakers.sideA else speakers.sideB).copy(alpha = 0.18f),
                        ),
                    )
                }
            }
            Spacer(Modifier.height(12.dp))
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("번역할 문장을 입력하세요") },
                minLines = 3,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { if (text.isNotBlank()) onSubmit(text, side) }),
            )
            Spacer(Modifier.height(12.dp))
            Button(
                onClick = { if (text.isNotBlank()) onSubmit(text, side) },
                enabled = text.isNotBlank() && !state.working,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text("번역", fontWeight = FontWeight.SemiBold)
            }
        }
    }
}
