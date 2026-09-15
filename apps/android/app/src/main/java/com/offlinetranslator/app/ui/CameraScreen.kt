package com.offlinetranslator.app.ui

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.ImageDecoder
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.CloudDownload
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.NoPhotography
import androidx.compose.material.icons.filled.PhotoLibrary
import androidx.compose.material.icons.filled.RotateRight
import androidx.compose.material.icons.filled.StopCircle
import androidx.compose.material.icons.filled.TextFields
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.offlinetranslator.app.CameraPhase
import com.offlinetranslator.app.Lang
import com.offlinetranslator.app.MainViewModel
import com.offlinetranslator.app.OcrEngine
import com.offlinetranslator.app.R
import com.offlinetranslator.app.Side
import com.offlinetranslator.app.UiState
import com.offlinetranslator.app.mb
import com.offlinetranslator.app.ui.theme.LocalSpeakerColors
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Photo translation: point the camera at a sign, a menu or a page (or pick a photo), say which of
 * the two languages the text is in, and the text is read on the phone and translated into the
 * other language, painted over the photo where the text stands. The result also joins the
 * conversation, so it can be replayed or copied there.
 */
@Composable
fun CameraScreen(vm: MainViewModel, state: UiState, onBack: () -> Unit, onOpenAppSettings: () -> Unit) {
    val context = LocalContext.current
    val speakers = LocalSpeakerColors.current
    var side by remember { mutableStateOf(Side.A) }
    var granted by remember {
        mutableStateOf(ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED)
    }
    val requestCamera = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted = it }
    var pickError by remember { mutableStateOf<String?>(null) }
    val imageFailed = stringResource(R.string.camera_image_failed)
    val cameraError = stringResource(R.string.err_camera)
    // Quality over latency: the letters' edges are what the recogniser reads.
    val imageCapture = remember { ImageCapture.Builder().setCaptureMode(ImageCapture.CAPTURE_MODE_MAXIMIZE_QUALITY).build() }
    val takePhoto: () -> Unit = {
        imageCapture.takePicture(ContextCompat.getMainExecutor(context), object : ImageCapture.OnImageCapturedCallback() {
            override fun onCaptureSuccess(image: ImageProxy) {
                val bitmap = runCatching { OcrEngine.prepare(image.toBitmap(), image.imageInfo.rotationDegrees) }
                image.close()
                bitmap.onSuccess { vm.translateImage(it, side) }.onFailure { pickError = cameraError }
            }

            override fun onError(exception: ImageCaptureException) {
                pickError = exception.message ?: cameraError
            }
        })
    }

    val leave = {
        vm.resetCamera()
        onBack()
    }
    BackHandler(onBack = leave)

    // Decoding through ImageDecoder applies the photo's EXIF rotation; a software bitmap is
    // needed because Tesseract reads the pixels.
    val pickImage = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        val bitmap = runCatching {
            ImageDecoder.decodeBitmap(ImageDecoder.createSource(context.contentResolver, uri)) { decoder, info, _ ->
                decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                decoder.isMutableRequired = false
                val longest = maxOf(info.size.width, info.size.height)
                if (longest > 2400) decoder.setTargetSampleSize(longest / 2000)
            }
        }.getOrNull()
        if (bitmap == null) pickError = imageFailed else vm.translateImage(OcrEngine.prepare(bitmap), side)
    }

    val srcLang = if (side == Side.A) state.langA else state.langB
    val ocrModel = vm.ocrModelFor(srcLang)
    val ocrReady = srcLang in state.ocrInstalled

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        when (val phase = state.camera) {
            is CameraPhase.Preview -> {
                if (granted) Viewfinder(imageCapture, onError = { pickError = it })
                TopBar(onBack = leave)
                Column(Modifier.align(Alignment.BottomCenter).fillMaxWidth()) {
                    Surface(color = Color.Black.copy(alpha = 0.72f)) {
                        Column(Modifier.navigationBarsPadding().padding(horizontal = 20.dp, vertical = 16.dp)) {
                            if (!granted) {
                                PermissionCard(onRequest = { requestCamera.launch(Manifest.permission.CAMERA) }, onOpenSettings = onOpenAppSettings)
                                Spacer(Modifier.height(12.dp))
                            }
                            Text(
                                stringResource(R.string.camera_text_language),
                                style = MaterialTheme.typography.labelMedium,
                                color = Color.White.copy(alpha = 0.7f),
                            )
                            Spacer(Modifier.height(6.dp))
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                listOf(Side.A to state.langA, Side.B to state.langB).forEach { (s, code) ->
                                    FilterChip(
                                        selected = side == s,
                                        onClick = { side = s },
                                        label = { Text(Lang.of(code).name) },
                                        colors = FilterChipDefaults.filterChipColors(
                                            labelColor = Color.White.copy(alpha = 0.85f),
                                            selectedLabelColor = Color.White,
                                            selectedContainerColor = (if (s == Side.A) speakers.sideA else speakers.sideB).copy(alpha = 0.55f),
                                        ),
                                    )
                                }
                            }
                            if (!ocrReady) {
                                Spacer(Modifier.height(12.dp))
                                OcrDataCard(
                                    lang = srcLang,
                                    sizeBytes = ocrModel?.sizeBytes ?: 0L,
                                    available = ocrModel != null,
                                    downloading = state.ocrDownload?.takeIf { it.lang == srcLang }?.let { d ->
                                        if (d.bytesTotal > 0) (d.bytesDone.toFloat() / d.bytesTotal).coerceIn(0f, 1f) else 0f
                                    },
                                    onDownload = { vm.downloadOcr(srcLang) },
                                )
                            }
                            Spacer(Modifier.height(16.dp))
                            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                                IconButton(onClick = { pickImage.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)) }, enabled = ocrReady) {
                                    Icon(
                                        Icons.Filled.PhotoLibrary,
                                        stringResource(R.string.camera_gallery),
                                        tint = if (ocrReady) Color.White else Color.White.copy(alpha = 0.35f),
                                        modifier = Modifier.size(28.dp),
                                    )
                                }
                                Spacer(Modifier.weight(1f))
                                Shutter(enabled = granted && ocrReady, onClick = takePhoto)
                                Spacer(Modifier.weight(1f))
                                Spacer(Modifier.width(48.dp))
                            }
                            Spacer(Modifier.height(4.dp))
                            Text(
                                stringResource(R.string.camera_hint),
                                style = MaterialTheme.typography.labelMedium,
                                color = Color.White.copy(alpha = 0.6f),
                                textAlign = TextAlign.Center,
                                modifier = Modifier.fillMaxWidth(),
                            )
                        }
                    }
                }
            }
            is CameraPhase.Working -> {
                Photo(phase.image)
                TopBar(onBack = leave)
                Column(
                    Modifier.align(Alignment.Center).background(Color.Black.copy(alpha = 0.6f), RoundedCornerShape(18.dp)).padding(24.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    CircularProgressIndicator(color = Color.White, strokeWidth = 3.dp, modifier = Modifier.size(36.dp))
                    Spacer(Modifier.height(14.dp))
                    Text(
                        when {
                            !phase.translating -> stringResource(R.string.camera_reading)
                            phase.total > 1 -> stringResource(R.string.camera_translating_progress, phase.done, phase.total)
                            else -> stringResource(R.string.translating)
                        },
                        style = MaterialTheme.typography.bodyLarge,
                        color = Color.White,
                    )
                }
            }
            is CameraPhase.Result -> {
                var showOriginal by remember(phase) { mutableStateOf(false) }
                var showText by remember(phase) { mutableStateOf(false) }
                Column(Modifier.fillMaxSize()) {
                    Box(Modifier.fillMaxWidth().weight(1f)) {
                        TranslatedPhoto(phase.image, phase.texts, showOriginal)
                        TopBar(onBack = leave) {
                            IconButton(onClick = vm::rotateCameraImage) {
                                Icon(Icons.Filled.RotateRight, stringResource(R.string.camera_rotate), tint = Color.White)
                            }
                            Spacer(Modifier.width(4.dp))
                            FilterChip(
                                selected = showOriginal,
                                onClick = { showOriginal = !showOriginal },
                                label = { Text(stringResource(R.string.camera_show_original)) },
                                colors = FilterChipDefaults.filterChipColors(
                                    containerColor = Color.Black.copy(alpha = 0.45f),
                                    labelColor = Color.White,
                                    selectedContainerColor = Color.White,
                                    selectedLabelColor = Color.Black,
                                ),
                            )
                        }
                    }
                    Surface(color = MaterialTheme.colorScheme.background, modifier = Modifier.fillMaxWidth()) {
                        Column(Modifier.navigationBarsPadding().padding(horizontal = 20.dp).padding(top = 12.dp)) {
                            if (showText) {
                                val none = stringResource(R.string.no_translation)
                                Column(Modifier.heightIn(max = 320.dp).verticalScroll(rememberScrollState())) {
                                    phase.texts.forEach { piece ->
                                        Text(
                                            piece.region.text,
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        )
                                        Spacer(Modifier.height(2.dp))
                                        Text(
                                            piece.translation.ifBlank { none },
                                            style = MaterialTheme.typography.bodyLarge,
                                            color = MaterialTheme.colorScheme.onSurface,
                                        )
                                        Spacer(Modifier.height(12.dp))
                                    }
                                }
                            } else {
                                Text(
                                    stringResource(R.string.camera_tap_hint),
                                    style = MaterialTheme.typography.labelMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                                Spacer(Modifier.height(4.dp))
                            }
                            Text(
                                stringResource(
                                    R.string.turn_footer,
                                    Lang.of(phase.turn.sourceLang).name,
                                    Lang.of(phase.turn.targetLang).name,
                                    phase.turn.totalMs.toInt(),
                                ),
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.outline,
                            )
                            Row(Modifier.fillMaxWidth().padding(top = 6.dp, bottom = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                                val speaking = state.speakingTurn == phase.turn.id
                                IconButton(onClick = { if (speaking) vm.stopSpeaking() else vm.speakTurn(phase.turn) }) {
                                    Icon(
                                        if (speaking) Icons.Filled.StopCircle else Icons.Filled.VolumeUp,
                                        stringResource(if (speaking) R.string.stop_playback else R.string.replay),
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                                IconButton(onClick = { showText = !showText }) {
                                    Icon(
                                        Icons.Filled.TextFields,
                                        stringResource(R.string.camera_text_list),
                                        tint = if (showText) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                                Spacer(Modifier.width(4.dp))
                                OutlinedButton(onClick = vm::resetCamera, modifier = Modifier.weight(1f)) {
                                    Text(stringResource(R.string.camera_retake))
                                }
                                Spacer(Modifier.width(10.dp))
                                Button(onClick = leave, modifier = Modifier.weight(1f)) {
                                    Text(stringResource(R.string.camera_done), fontWeight = FontWeight.SemiBold)
                                }
                            }
                        }
                    }
                }
            }
            is CameraPhase.Failed -> {
                phase.image?.let { Photo(it) }
                TopBar(onBack = leave)
                Column(
                    Modifier.align(Alignment.Center).padding(28.dp).background(Color.Black.copy(alpha = 0.72f), RoundedCornerShape(18.dp)).padding(22.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Icon(Icons.Filled.NoPhotography, null, tint = Color.White.copy(alpha = 0.8f), modifier = Modifier.size(30.dp))
                    Spacer(Modifier.height(12.dp))
                    Text(phase.message, style = MaterialTheme.typography.bodyMedium, color = Color.White, textAlign = TextAlign.Center)
                    Spacer(Modifier.height(18.dp))
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (phase.image != null) {
                            OutlinedButton(onClick = vm::rotateCameraImage) {
                                Icon(Icons.Filled.RotateRight, null, tint = Color.White, modifier = Modifier.size(18.dp))
                                Spacer(Modifier.width(6.dp))
                                Text(stringResource(R.string.camera_rotate_short), color = Color.White)
                            }
                            Spacer(Modifier.width(10.dp))
                        }
                        Button(onClick = vm::resetCamera) { Text(stringResource(R.string.camera_retake)) }
                    }
                }
            }
        }

        pickError?.let { msg ->
            LaunchedEffect(msg) {
                vm.showMessage(msg)
                pickError = null
            }
        }
    }
}

/** CameraX preview bound to the screen's lifecycle together with [imageCapture]. */
@Composable
private fun Viewfinder(imageCapture: ImageCapture, onError: (String) -> Unit) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val previewView = remember { PreviewView(context).apply { scaleType = PreviewView.ScaleType.FILL_CENTER } }
    val cameraError = stringResource(R.string.err_camera)
    var provider by remember { mutableStateOf<ProcessCameraProvider?>(null) }

    LaunchedEffect(Unit) {
        val p = runCatching { withContext(Dispatchers.IO) { ProcessCameraProvider.getInstance(context).get() } }.getOrNull()
        if (p == null) {
            onError(cameraError)
            return@LaunchedEffect
        }
        val preview = Preview.Builder().build().also { it.setSurfaceProvider(previewView.surfaceProvider) }
        runCatching {
            p.unbindAll()
            p.bindToLifecycle(lifecycleOwner, CameraSelector.DEFAULT_BACK_CAMERA, preview, imageCapture)
        }.onFailure { onError(cameraError) }
        provider = p
    }
    DisposableEffect(Unit) {
        onDispose { runCatching { provider?.unbindAll() } }
    }

    AndroidView(factory = { previewView }, modifier = Modifier.fillMaxSize())
}

@Composable
private fun Shutter(enabled: Boolean, onClick: () -> Unit) {
    Box(
        Modifier
            .size(74.dp)
            .clip(CircleShape)
            .border(4.dp, if (enabled) Color.White else Color.White.copy(alpha = 0.35f), CircleShape)
            .padding(7.dp)
            .clip(CircleShape)
            .background(if (enabled) Color.White else Color.White.copy(alpha = 0.25f))
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(Icons.Filled.CameraAlt, stringResource(R.string.camera_shutter), tint = Color.Black.copy(alpha = if (enabled) 0.85f else 0.4f))
    }
}

@Composable
private fun Photo(image: Bitmap) {
    Image(
        bitmap = image.asImageBitmap(),
        contentDescription = null,
        contentScale = ContentScale.Fit,
        modifier = Modifier.fillMaxSize(),
    )
}

/** Back arrow and title over the photo, on a shade so they read on a bright picture; [actions] at the end. */
@Composable
private fun TopBar(onBack: () -> Unit, actions: @Composable RowScope.() -> Unit = {}) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Brush.verticalGradient(listOf(Color.Black.copy(alpha = 0.55f), Color.Transparent)))
            .statusBarsPadding()
            .padding(start = 4.dp, end = 16.dp, top = 4.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconButton(onClick = onBack) {
            Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back), tint = Color.White)
        }
        Text(stringResource(R.string.camera_title), style = MaterialTheme.typography.titleMedium, color = Color.White)
        Spacer(Modifier.weight(1f))
        actions()
    }
}

@Composable
private fun PermissionCard(onRequest: () -> Unit, onOpenSettings: () -> Unit) {
    Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.surfaceVariant, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text(stringResource(R.string.camera_needed_title), style = MaterialTheme.typography.labelLarge)
            Spacer(Modifier.height(6.dp))
            Text(
                stringResource(R.string.camera_needed_body),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(12.dp))
            Row {
                Button(onClick = onRequest) { Text(stringResource(R.string.mic_allow)) }
                Spacer(Modifier.width(8.dp))
                TextButton(onClick = onOpenSettings) { Text(stringResource(R.string.mic_open_settings)) }
            }
        }
    }
}

/** Text recognition for the chosen language is not on the phone yet: offer the download, show it running. */
@Composable
private fun OcrDataCard(lang: String, sizeBytes: Long, available: Boolean, downloading: Float?, onDownload: () -> Unit) {
    Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.surfaceVariant, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text(stringResource(R.string.ocr_model_title, Lang.of(lang).name), style = MaterialTheme.typography.labelLarge)
            Spacer(Modifier.height(6.dp))
            when {
                !available -> Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Filled.ErrorOutline, null, tint = MaterialTheme.colorScheme.error, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Text(
                        stringResource(R.string.ocr_missing_catalog),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                downloading != null -> {
                    Text(
                        stringResource(R.string.ocr_downloading),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(8.dp))
                    LinearProgressIndicator(progress = { downloading }, modifier = Modifier.fillMaxWidth().height(6.dp))
                }
                else -> {
                    Text(
                        stringResource(R.string.ocr_download_body, Lang.of(lang).name, mb(sizeBytes)),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(12.dp))
                    Button(onClick = onDownload) {
                        Icon(Icons.Filled.CloudDownload, null, Modifier.size(18.dp))
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.setup_download, mb(sizeBytes)))
                    }
                }
            }
        }
    }
}
