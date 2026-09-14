package com.offlinetranslator.app

import android.graphics.Bitmap
import android.graphics.Matrix
import com.googlecode.tesseract.android.TessBaseAPI
import java.io.File

/**
 * Reads the text in a photo with Tesseract (tesseract4android) and turns its line-by-line output
 * into paragraphs the translator can work with.
 */
object OcrEngine {
    /**
     * Longest side handed to Tesseract. Phone photos are 3000–4000 px; that much costs ten
     * seconds and helps nothing, since the recogniser was trained at roughly 30 px per line.
     */
    private const val MAX_SIDE = 2000

    /** Languages written without spaces between words: line breaks join with nothing, and Tesseract's spaces between characters go. */
    private val unspaced = setOf("ja", "zh", "th")

    /** Recognises [lang] text in [image]; [dataRoot] holds `tessdata/`. Blank when nothing readable was found. */
    fun recognize(dataRoot: File, image: Bitmap, lang: String, tessLang: String): String {
        val tess = TessBaseAPI()
        try {
            if (!tess.init(dataRoot.absolutePath, tessLang)) throw IllegalStateException("tesseract init failed for $tessLang")
            tess.setPageSegMode(TessBaseAPI.PageSegMode.PSM_AUTO)
            tess.setImage(prepare(image))
            val raw = tess.getUTF8Text() ?: ""
            return tidy(raw, lang)
        } finally {
            tess.recycle()
        }
    }

    /** Scales a photo down to [MAX_SIDE] and makes sure Tesseract can read its pixels (no hardware bitmaps). */
    fun prepare(image: Bitmap, rotationDegrees: Int = 0): Bitmap {
        val longest = maxOf(image.width, image.height)
        val scale = if (longest > MAX_SIDE) MAX_SIDE.toFloat() / longest else 1f
        val m = Matrix()
        if (scale < 1f) m.postScale(scale, scale)
        if (rotationDegrees != 0) m.postRotate(rotationDegrees.toFloat())
        val out = if (m.isIdentity) image else Bitmap.createBitmap(image, 0, 0, image.width, image.height, m, true)
        return if (out.config == Bitmap.Config.ARGB_8888) out else out.copy(Bitmap.Config.ARGB_8888, false)
    }

    /**
     * Tesseract returns one line per text line, a blank line between blocks, and (for Chinese,
     * Japanese and Thai) a space between characters. Lines of a block become one paragraph, so a
     * sentence wrapped over three lines of a sign is translated as one sentence; blocks stay
     * separate lines. Lines without a single letter or digit are noise from edges and textures.
     */
    fun tidy(raw: String, lang: String): String {
        val noSpaces = lang in unspaced
        val paragraphs = raw.replace("\r", "").split(Regex("\n\\s*\n"))
        val out = ArrayList<String>()
        for (block in paragraphs) {
            val lines = block.split('\n')
                .map { it.replace(Regex("[ \\t\\u00A0]+"), " ").trim() }
                .filter { line -> line.any { it.isLetterOrDigit() } }
            if (lines.isEmpty()) continue
            val sb = StringBuilder()
            for (line in lines) {
                if (sb.isNotEmpty()) {
                    val last = sb.last()
                    val first = line.first()
                    when {
                        // "trans-\nlation": a hyphen at the end of a line, then a lower-case letter.
                        !noSpaces && last == '-' && sb.length > 1 && sb[sb.length - 2].isLetter() && first.isLowerCase() ->
                            sb.setLength(sb.length - 1)
                        noSpaces && !(last.isAscii() && first.isAscii()) -> {}
                        else -> sb.append(' ')
                    }
                }
                sb.append(line)
            }
            var text = sb.toString()
            if (noSpaces) {
                // Spaces between two non-ASCII characters are Tesseract's, not the writer's.
                text = text.replace(Regex("(?<=[^\\x00-\\x7F]) +(?=[^\\x00-\\x7F])"), "")
            }
            out += text
        }
        return out.joinToString("\n")
    }

    private fun Char.isAscii(): Boolean = code < 0x80
}
