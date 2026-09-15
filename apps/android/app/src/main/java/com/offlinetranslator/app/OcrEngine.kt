package com.offlinetranslator.app

import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Rect
import com.googlecode.tesseract.android.TessBaseAPI
import com.googlecode.tesseract.android.TessBaseAPI.PageIteratorLevel
import java.io.File
import java.util.Locale
import kotlin.math.roundToInt

/**
 * One piece of text in a photo, in reading order: what it says, where it stands (pixels of
 * [OcrPage.image]) and the colours a translation is painted over it with.
 */
data class OcrRegion(
    val text: String,
    val box: Rect,
    /** Height of one of its lines in pixels; the translation's letters are sized from it. */
    val lineHeight: Int,
    val lineCount: Int,
    /** The surface around the text (ARGB), and black or white, whichever reads on it. */
    val background: Int,
    val foreground: Int,
)

/** What was read from a photo: the photo as it was read (turned upright when it stood sideways) and its text. */
data class OcrPage(val image: Bitmap, val regions: List<OcrRegion>)

/**
 * Reads the text in a photo with Tesseract (tesseract4android) as positioned lines and groups them
 * into the pieces the translator works with: a sentence wrapped over three lines is one piece, the
 * items of a menu or the lines of a sign stay separate pieces.
 */
object OcrEngine {
    /**
     * Longest side handed to Tesseract. Phone photos are 3000–4000 px; that much costs ten
     * seconds and helps nothing, since the recogniser was trained at roughly 30 px per line.
     */
    private const val MAX_SIDE = 2000

    /** Longest side of the small copies read to find which way round a photo stands. */
    private const val PROBE_SIDE = 1000

    /** Languages written without spaces between words: line breaks join with nothing, and Tesseract's spaces between characters go. */
    private val unspaced = setOf("ja", "zh", "th")

    /** Languages with capital letters. */
    private val cased = setOf("en", "es", "fr", "id", "vi", "ru")

    /**
     * Words Tesseract is less sure of than this (0–100) are dropped, and lines whose letters average
     * below [MIN_LINE_CONFIDENCE]: edges, textures and glare read as short runs of letters, which the
     * translator then turned into invented words.
     */
    private const val MIN_WORD_CONFIDENCE = 30f
    private const val MIN_LINE_CONFIDENCE = 45f

    /**
     * A photo reads the right way round when at least [UPRIGHT_LETTERS] letters, and
     * [UPRIGHT_SHARE] of all it read, come from words of confidence [CONFIDENT] or more on lines
     * that run across. Otherwise the four ways round are compared on small copies and the photo is
     * turned when another one gives [TURN_MARGIN] times those letters (`tests/eval/ocr_eval.py`).
     */
    private const val CONFIDENT = 70f
    private const val UPRIGHT_LETTERS = 20
    private const val UPRIGHT_SHARE = 0.5f
    private const val TURN_MARGIN = 1.5f

    /** Lines lower than this many pixels are specks, not text. */
    private const val MIN_LINE_HEIGHT = 8

    /**
     * Recognises [lang] text in [image]; [dataRoot] holds `tessdata/`. With [findOrientation] a
     * photo that does not read well as it stands is also read turned a quarter, a half and three
     * quarters, and the way round Tesseract is surest of wins. No regions when nothing readable
     * was found.
     */
    fun recognize(dataRoot: File, image: Bitmap, lang: String, tessLang: String, findOrientation: Boolean = true): OcrPage {
        val tess = TessBaseAPI()
        try {
            if (!tess.init(dataRoot.absolutePath, tessLang)) throw IllegalStateException("tesseract init failed for $tessLang")
            tess.setPageSegMode(TessBaseAPI.PageSegMode.PSM_AUTO)
            val asItStands = read(tess, image, lang)
            if (!findOrientation || asItStands.readsWell()) return page(image, asItStands, lang)
            val turn = orientation(tess, image, lang)
            if (turn == 0) return page(image, asItStands, lang)
            val turned = prepare(image, turn)
            return page(turned, read(tess, turned, lang), lang)
        } finally {
            tess.recycle()
        }
    }

    /** Scales a photo down to [MAX_SIDE], turns it clockwise by [rotationDegrees], and makes sure Tesseract can read its pixels (no hardware bitmaps). */
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
     * The text handed to the translator. Signs are often written in capitals, which OPUS-MT has
     * rarely seen: of 50 sign texts through the app's pairs (en→ko, fr/es/ru→en), 16 came out right
     * in capitals and 36 in sentence case (`tests/eval/ocr_caps_eval.py`).
     */
    fun forTranslation(text: String, lang: String): String {
        if (lang !in cased || !text.isAllCaps()) return text
        val locale = Locale.forLanguageTag(lang)
        return text.lowercase(locale).replaceFirstChar { it.titlecase(locale) }
    }

    /**
     * Whether [text] is language rather than a code, a date, a price or a phone number
     * ("10SEP26 B0066", "DL/CC", "$ 42.600", "387 4217474"): Marian answered those with invented
     * words, so they stay as they are. Language has a run of two or more letters with no digit
     * in it and more letters than digits.
     */
    fun isTranslatable(text: String, lang: String): Boolean {
        var letters = 0
        var digits = 0
        var word = false
        var run = 0
        var runHasDigit = false
        fun endRun() {
            if (run >= 2 && !runHasDigit) word = true
            run = 0
            runHasDigit = false
        }
        for (c in text) {
            val type = Character.getType(c)
            when {
                c.isLetter() -> { letters++; run++ }
                c.isDigit() -> { digits++; run++; runHasDigit = true }
                // Vowel signs and tone marks (Thai, Vietnamese) belong to the letter before them.
                type == Character.NON_SPACING_MARK.toInt() || type == Character.COMBINING_SPACING_MARK.toInt() -> {}
                c == '\'' || c == '’' -> {}
                else -> endRun()
            }
        }
        endRun()
        if (lang in unspaced) return letters >= 2 && letters > digits
        return word && letters > digits
    }

    private class Word(val text: String, val confidence: Float, val box: Rect)

    /** A recognised text line; [para] numbers Tesseract's paragraphs across the page. */
    private class Line(val text: String, val box: Rect, val para: Int)

    /**
     * One pass of Tesseract over a bitmap: its lines, how many letters it read, and how many of them
     * it read confidently on lines that run across the bitmap.
     */
    private class Reading(val lines: List<Line>, val letters: Int, val confidentLetters: Int) {
        fun readsWell() = confidentLetters >= UPRIGHT_LETTERS && confidentLetters >= letters * UPRIGHT_SHARE
    }

    /** Clockwise quarter turns (0, 90, 180, 270) that make [image] read best, judged on small copies. */
    private fun orientation(tess: TessBaseAPI, image: Bitmap, lang: String): Int {
        val scale = PROBE_SIDE.toFloat() / maxOf(image.width, image.height)
        val small = if (scale < 1f) {
            Bitmap.createScaledBitmap(image, (image.width * scale).roundToInt(), (image.height * scale).roundToInt(), true)
        } else {
            image
        }
        val scores = IntArray(4) { i ->
            val probe = if (i == 0) small else prepare(small, i * 90)
            val score = read(tess, probe, lang).confidentLetters
            if (probe !== small) probe.recycle()
            score
        }
        if (small !== image) small.recycle()
        val best = scores.indices.maxBy { scores[it] }
        return if (best != 0 && scores[best] >= UPRIGHT_LETTERS && scores[best] > scores[0] * TURN_MARGIN) best * 90 else 0
    }

    private fun page(image: Bitmap, reading: Reading, lang: String) =
        OcrPage(image, group(reading.lines).map { region(it, image, lang) })

    private fun read(tess: TessBaseAPI, image: Bitmap, lang: String): Reading {
        tess.setImage(image)
        tess.getUTF8Text() // runs layout analysis and recognition; the iterator reads the result
        val iter = tess.resultIterator ?: return Reading(emptyList(), 0, 0)
        val lines = ArrayList<Line>()
        val unfiltered = ArrayList<Line>()
        var letters = 0
        var confident = 0
        var para = -1
        var words = ArrayList<Word>()
        fun close() {
            if (words.isNotEmpty()) {
                // Tesseract also reads text turned a quarter clockwise, as lines running down; those
                // count as letters read but not as confident ones, so such a photo gets turned.
                val box = Rect(words[0].box)
                words.forEach { box.union(it.box) }
                val runsAcross = box.width() >= box.height()
                for (w in words) {
                    val n = w.text.count { it.isLetterOrDigit() }
                    letters += n
                    if (runsAcross && w.confidence >= CONFIDENT) confident += n
                }
                lineOf(words, para, lang, filter = true)?.let { line -> lines += line }
                lineOf(words, para, lang, filter = false)?.let { line -> unfiltered += line }
            }
            words = ArrayList()
        }
        try {
            iter.begin()
            do {
                if (iter.isAtBeginningOf(PageIteratorLevel.RIL_PARA)) {
                    close()
                    para++
                } else if (iter.isAtBeginningOf(PageIteratorLevel.RIL_TEXTLINE)) {
                    close()
                }
                val text = iter.getUTF8Text(PageIteratorLevel.RIL_WORD)?.trim()
                if (!text.isNullOrEmpty()) {
                    words += Word(text, iter.confidence(PageIteratorLevel.RIL_WORD), iter.getBoundingRect(PageIteratorLevel.RIL_WORD))
                }
            } while (iter.next(PageIteratorLevel.RIL_WORD))
            close()
        } finally {
            iter.delete()
        }
        // When the confidence bars leave nothing, what was read is still better than "no text".
        return Reading(lines.ifEmpty { unfiltered }, letters, confident)
    }

    /** The line made of [words], or null when it is noise; [filter] applies the confidence bars. */
    private fun lineOf(words: List<Word>, para: Int, lang: String, filter: Boolean): Line? {
        if (filter) {
            val chars = words.sumOf { it.text.length }
            val mean = words.sumOf { (it.confidence * it.text.length).toDouble() } / maxOf(1, chars)
            if (mean < MIN_LINE_CONFIDENCE) return null
        }
        val kept = if (filter) words.filter { it.confidence >= MIN_WORD_CONFIDENCE } else words
        if (kept.none { w -> w.text.any { it.isLetterOrDigit() } }) return null
        val box = Rect(kept[0].box)
        kept.forEach { box.union(it.box) }
        if (box.height() < MIN_LINE_HEIGHT) return null
        var text = kept.joinToString(" ") { it.text }
        if (lang in unspaced) text = dropCharacterSpaces(text)
        return Line(text, box, para)
    }

    /**
     * Consecutive lines of one paragraph that belong to one sentence. Tesseract puts a menu's items
     * or a sign's lines into one paragraph; translated as one sentence they came out as nonsense.
     */
    private fun group(lines: List<Line>): List<List<Line>> {
        val extent = HashMap<Int, Rect>()
        for (l in lines) extent.getOrPut(l.para) { Rect(l.box) }.union(l.box)
        val out = ArrayList<MutableList<Line>>()
        for (line in lines) {
            val cur = out.lastOrNull()
            if (cur != null && continues(cur.last(), line, extent.getValue(line.para))) cur += line else out += mutableListOf(line)
        }
        return out
    }

    /** Whether [next] carries on the sentence of [prev]; [para] is the paragraph's extent. */
    private fun continues(prev: Line, next: Line, para: Rect): Boolean {
        if (prev.para != next.para) return false
        val ph = prev.box.height().toFloat()
        val nh = next.box.height().toFloat()
        val h = (ph + nh) / 2
        // A heading over body text, a gap, or text side by side rather than below.
        if (nh < 0.7f * ph || nh > 1.4f * ph) return false
        val gap = next.box.top - prev.box.bottom
        if (gap > 0.9f * h || gap < -0.5f * h) return false
        if (next.box.left >= prev.box.right || next.box.right <= prev.box.left) return false

        val last = prev.text.last()
        val first = next.text.first()
        // A price, a number or a finished sentence ends the piece.
        if (last.isDigit() || last in ".!?:;。！？：；%)") return false
        if (last == '-' || last == ',' || last == '、' || last == '，') return true
        // Cased scripts: a lower-case start carries a sentence on; lines all in capitals are a sign.
        if (first.isLowerCase()) return true
        if (prev.text.isAllCaps() && next.text.isAllCaps()) return true
        // Otherwise a line wrapped only if it ran to the paragraph's right edge.
        return prev.box.right >= para.right - 1.5f * h
    }

    private fun region(lines: List<Line>, image: Bitmap, lang: String): OcrRegion {
        val box = Rect(lines[0].box)
        lines.forEach { box.union(it.box) }
        val heights = lines.map { it.box.height() }.sorted()
        val lineHeight = heights[heights.size / 2]
        val background = backgroundAround(image, box, maxOf(2, lineHeight / 4))
        val foreground = if (Color.luminance(background) > 0.18f) Color.rgb(20, 20, 20) else Color.WHITE
        return OcrRegion(joinLines(lines.map { it.text }, lang), box, lineHeight, lines.size, background, foreground)
    }

    /** Median colour of a thin ring just outside [box]: the surface the text is printed on. */
    private fun backgroundAround(image: Bitmap, box: Rect, margin: Int): Int {
        val ring = Rect(box.left - margin, box.top - margin, box.right + margin, box.bottom + margin)
        if (!ring.intersect(0, 0, image.width, image.height)) return Color.WHITE
        val r = ArrayList<Int>()
        val g = ArrayList<Int>()
        val b = ArrayList<Int>()
        fun sample(x: Int, y: Int) {
            val c = image.getPixel(x, y)
            r += Color.red(c)
            g += Color.green(c)
            b += Color.blue(c)
        }
        val stride = maxOf(1, (ring.width() + ring.height()) / 80)
        for (x in ring.left until ring.right step stride) {
            sample(x, ring.top)
            sample(x, ring.bottom - 1)
        }
        for (y in ring.top until ring.bottom step stride) {
            sample(ring.left, y)
            sample(ring.right - 1, y)
        }
        fun median(v: ArrayList<Int>): Int = v.sorted()[v.size / 2]
        return Color.rgb(median(r), median(g), median(b))
    }

    /**
     * The lines of one piece as one paragraph, so a sentence wrapped over three lines of a sign is
     * translated as one sentence: "trans-" + "lation" is mended, lines of Chinese, Japanese and
     * Thai join without a space.
     */
    fun joinLines(lines: List<String>, lang: String): String {
        val noSpaces = lang in unspaced
        val sb = StringBuilder()
        for (line in lines) {
            if (sb.isNotEmpty()) {
                val last = sb.last()
                val first = line.first()
                when {
                    !noSpaces && last == '-' && sb.length > 1 && sb[sb.length - 2].isLetter() && first.isLowerCase() ->
                        sb.setLength(sb.length - 1)
                    noSpaces && !(last.isAscii() && first.isAscii()) -> {}
                    else -> sb.append(' ')
                }
            }
            sb.append(line)
        }
        return sb.toString()
    }

    /** Spaces between two non-ASCII characters are Tesseract's, not the writer's (Chinese, Japanese, Thai). */
    private fun dropCharacterSpaces(text: String): String =
        text.replace(Regex("(?<=[^\\x00-\\x7F]) +(?=[^\\x00-\\x7F])"), "")

    /** At least two letters, every cased letter a capital. */
    private fun String.isAllCaps(): Boolean {
        var letters = 0
        for (c in this) {
            if (c.isLowerCase()) return false
            if (c.isUpperCase()) letters++
        }
        return letters >= 2
    }

    private fun Char.isAscii(): Boolean = code < 0x80
}
