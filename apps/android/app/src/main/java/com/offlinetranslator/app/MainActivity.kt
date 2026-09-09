package com.offlinetranslator.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.offlinetranslator.app.ui.InterpreterApp
import com.offlinetranslator.app.ui.theme.InterpreterTheme
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    private val vm: MainViewModel by viewModels()
    private val micGranted = MutableStateFlow(false)

    private val requestMic = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.RequestPermission()
    ) { granted -> micGranted.value = granted }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Edge to edge: every screen applies its own status/navigation bar padding.
        WindowCompat.setDecorFitsSystemWindows(window, false)
        micGranted.value = hasMic()

        // A conversation is held with the screen facing the other person; do not let it sleep.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        setContent {
            InterpreterTheme {
                InterpreterApp(
                    vm = vm,
                    micGranted = micGranted,
                    onRequestMic = { requestMic.launch(Manifest.permission.RECORD_AUDIO) },
                    onOpenAppSettings = { openAppSettings() },
                )
            }
        }

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.RESUMED) {
                micGranted.value = hasMic()
                vm.resumeMic()
                try {
                    kotlinx.coroutines.awaitCancellation()
                } finally {
                    vm.pauseMic()
                }
            }
        }
    }

    private fun hasMic() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private fun openAppSettings() {
        startActivity(
            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null))
        )
    }
}
