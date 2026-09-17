package com.offlinetranslator.app.ui

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.offlinetranslator.app.PhotoText
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

private const val MAX_ZOOM = 5f
private const val DOUBLE_TAP_ZOOM = 2.5f

/**
 * The photo with each piece of text covered by its translation, where the text stands: a patch in
 * the colour of the surface around it, and letters as large as still fit the original's box.
 * Pinch or double-tap zooms; tapping a piece uncovers the original under it, [showOriginal]
 * uncovers all of them.
 */
@Composable
fun TranslatedPhoto(image: Bitmap, texts: List<PhotoText>, showOriginal: Boolean, modifier: Modifier = Modifier) {
    val bitmap = remember(image) { image.asImageBitmap() }
    val measurer = rememberTextMeasurer()
    val labels = remember(texts) { texts.mapNotNull { label(measurer, it) } }
    var peeked by remember(texts) { mutableStateOf(emptySet<Label>()) }
    var zoom by remember(image) { mutableFloatStateOf(1f) }
    var pan by remember(image) { mutableStateOf(Offset.Zero) }

    Canvas(
        modifier
            .fillMaxSize()
            .clipToBounds()
            .pointerInput(image) {
                detectTransformGestures { centroid, panBy, zoomBy, _ ->
                    val z = (zoom * zoomBy).coerceIn(1f, MAX_ZOOM)
                    pan = clampPan(centroid - (centroid - pan) * (z / zoom) + panBy, z, size)
                    zoom = z
                }
            }
            .pointerInput(labels) {
                detectTapGestures(
                    onDoubleTap = { at ->
                        if (zoom > 1.01f) {
                            zoom = 1f
                            pan = Offset.Zero
                        } else {
                            zoom = DOUBLE_TAP_ZOOM
                            pan = clampPan(at * (1 - DOUBLE_TAP_ZOOM), DOUBLE_TAP_ZOOM, size)
                        }
                    },
                    onTap = { at ->
                        val (fit, origin) = fitOf(size.width.toFloat(), size.height.toFloat(), image)
                        val onPhoto = ((at - pan) / zoom - origin) / fit
                        labels.lastOrNull { it.area.contains(onPhoto) }?.let { hit ->
                            peeked = if (hit in peeked) peeked - hit else peeked + hit
                        }
                    },
                )
            },
    ) {
        val (fit, origin) = fitOf(size.width, size.height, image)
        val k = fit * zoom
        val topLeft = pan + origin * zoom
        drawImage(
            image = bitmap,
            dstOffset = IntOffset(topLeft.x.roundToInt(), topLeft.y.roundToInt()),
            dstSize = IntSize((image.width * k).roundToInt(), (image.height * k).roundToInt()),
            filterQuality = FilterQuality.Medium,
        )
        val outline = Stroke(width = 2.dp.toPx())
        for (l in labels) {
            val area = Rect(topLeft + l.area.topLeft * k, l.area.size * k)
            if (showOriginal) continue
            if (l in peeked) {
                drawRoundRect(color = Color.White, topLeft = area.topLeft, size = area.size, cornerRadius = CornerRadius(4.dp.toPx()), style = outline)
                continue
            }
            drawRoundRect(color = l.background, topLeft = area.topLeft, size = area.size, cornerRadius = CornerRadius(min(3.dp.toPx(), area.height / 4)))
            withTransform({
                translate(topLeft.x + l.textAt.x * k, topLeft.y + l.textAt.y * k)
                scale(k, k, pivot = Offset.Zero)
            }) {
                drawText(l.layout, color = l.foreground)
            }
        }
    }
}

/**
 * Live mode's overlay: the translations of the last frame painted over the viewfinder. The frame
 * the recogniser read is [frameWidth] x [frameHeight] and the viewfinder shows the same picture
 * cropped to the screen (PreviewView.FILL_CENTER), so the boxes follow that crop. Nothing to tap
 * and nothing to zoom: the scene moves under them, and a photo is what can be examined.
 */
@Composable
fun LiveOverlay(texts: List<PhotoText>, frameWidth: Int, frameHeight: Int, modifier: Modifier = Modifier) {
    if (frameWidth <= 0 || frameHeight <= 0) return
    val measurer = rememberTextMeasurer()
    val labels = remember(texts) { texts.mapNotNull { label(measurer, it) } }
    Canvas(modifier.fillMaxSize().clipToBounds()) {
        val k = max(size.width / frameWidth, size.height / frameHeight)
        val origin = Offset((size.width - frameWidth * k) / 2, (size.height - frameHeight * k) / 2)
        for (l in labels) {
            val area = Rect(origin + l.area.topLeft * k, l.area.size * k)
            drawRoundRect(
                color = l.background,
                topLeft = area.topLeft,
                size = area.size,
                cornerRadius = CornerRadius(min(3.dp.toPx(), area.height / 4)),
            )
            withTransform({
                translate(origin.x + l.textAt.x * k, origin.y + l.textAt.y * k)
                scale(k, k, pivot = Offset.Zero)
            }) {
                drawText(l.layout, color = l.foreground)
            }
        }
    }
}

/** A translation laid out in photo pixels: the patch it covers, where its text starts, its colours. */
internal class Label(
    val area: Rect,
    val textAt: Offset,
    val layout: TextLayoutResult,
    val background: Color,
    val foreground: Color,
)

/** Text measured with one font unit to a pixel of the photo. */
private val PhotoPixels = Density(1f)

internal fun label(measurer: TextMeasurer, piece: PhotoText): Label? {
    val text = piece.translation
    if (text.isBlank()) return null
    val r = piece.region
    val box = Rect(r.box.left.toFloat(), r.box.top.toFloat(), r.box.right.toFloat(), r.box.bottom.toFloat())
    val lineHeight = r.lineHeight.toFloat().coerceAtLeast(8f)
    val width = box.width.roundToInt().coerceAtLeast(1)
    fun measure(s: String, size: Float, maxWidth: Int) = measurer.measure(
        AnnotatedString(s),
        style = TextStyle(fontSize = size.sp, lineHeight = (size * 1.15f).sp, fontWeight = FontWeight.Medium),
        constraints = Constraints(maxWidth = maxWidth),
        density = PhotoPixels,
        skipCache = true,
    )

    // The longest word must fit on a line, or the translation breaks inside a word. Thai, kana and
    // Chinese characters break anywhere.
    val breaksAnywhere = text.any { it.code in 0x0E00..0x0E7F || it.code in 0x3040..0x9FFF }
    val wordWidth = if (breaksAnywhere) 0f else {
        val longest = text.split(' ', '\n').maxByOrNull { it.length }.orEmpty()
        measure(longest, 100f, Constraints.Infinity).size.width / 100f
    }
    fun fits(l: TextLayoutResult, size: Float) = l.size.height <= box.height * 1.1f && wordWidth * size <= width

    // As large as the original's letters, down to a third of them; below that the patch grows.
    var lo = lineHeight * 0.3f
    var hi = lineHeight * 0.8f
    var best = measure(text, hi, width)
    if (!fits(best, hi)) {
        best = measure(text, lo, width)
        repeat(6) {
            val mid = (lo + hi) / 2
            val l = measure(text, mid, width)
            if (fits(l, mid)) {
                best = l
                lo = mid
            } else {
                hi = mid
            }
        }
    }
    val pad = lineHeight * 0.15f
    val textHeight = best.size.height.toFloat()
    val textTop = box.top + (box.height - textHeight).coerceAtLeast(0f) / 2
    val area = Rect(
        box.left - pad,
        box.top - pad,
        maxOf(box.right, box.left + best.size.width) + pad,
        box.top + maxOf(box.height, textHeight) + pad,
    )
    return Label(area, Offset(box.left, textTop), best, Color(r.background), Color(r.foreground))
}

/** Scale and offset that fit [image] into a view of [width] × [height], centred. */
private fun fitOf(width: Float, height: Float, image: Bitmap): Pair<Float, Offset> {
    val s = min(width / image.width, height / image.height)
    return s to Offset((width - image.width * s) / 2, (height - image.height * s) / 2)
}

/** Keeps the zoomed photo covering the view. */
private fun clampPan(pan: Offset, zoom: Float, size: IntSize) =
    Offset(pan.x.coerceIn(size.width * (1 - zoom), 0f), pan.y.coerceIn(size.height * (1 - zoom), 0f))
