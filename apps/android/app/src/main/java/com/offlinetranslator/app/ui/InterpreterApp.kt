package com.offlinetranslator.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.offlinetranslator.app.MainViewModel
import com.offlinetranslator.app.Phase
import kotlinx.coroutines.flow.StateFlow

@Composable
fun InterpreterApp(
    vm: MainViewModel,
    micGranted: StateFlow<Boolean>,
    onRequestMic: () -> Unit,
    onOpenAppSettings: () -> Unit,
) {
    val state by vm.state.collectAsStateWithLifecycle()
    val mic by micGranted.collectAsStateWithLifecycle()
    var showSettings by remember { mutableStateOf(false) }

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        // Settings stays reachable during setup so the languages can be changed before a
        // 0.5 GB download starts.
        if (showSettings) {
            SettingsScreen(vm, state, onBack = { showSettings = false })
        } else {
            when (val phase = state.phase) {
                is Phase.Checking -> Busy("모델을 확인하는 중")
                is Phase.Opening -> Busy("통역 엔진을 준비하는 중")
                is Phase.Setup -> SetupScreen(state, phase, onDownload = vm::download, onOpenSettings = { showSettings = true })
                is Phase.Downloading -> DownloadScreen(phase, onCancel = vm::cancelDownload)
                is Phase.Fatal -> Failure(phase.message, onRetry = vm::boot)
                is Phase.Ready -> ConversationScreen(
                    vm = vm,
                    state = state,
                    micGranted = mic,
                    onRequestMic = onRequestMic,
                    onOpenAppSettings = onOpenAppSettings,
                    onOpenSettings = { showSettings = true },
                )
            }
        }
    }
}

@Composable
private fun Busy(message: String) {
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        CircularProgressIndicator(strokeWidth = 3.dp, modifier = Modifier.size(36.dp))
        Spacer(Modifier.height(20.dp))
        Text(message, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun Failure(message: String, onRetry: () -> Unit) {
    Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                "시작할 수 없습니다",
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onBackground,
            )
            Spacer(Modifier.height(10.dp))
            Text(
                message,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(24.dp))
            Button(onClick = onRetry, modifier = Modifier.fillMaxWidth()) { Text("다시 시도") }
        }
    }
}
