package com.offlinetranslator.app

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.media.ExifInterface
import android.net.Uri
import java.io.IOException

/**
 * Opens a photo the user picked (gallery, files, a cloud-backed picture) as an upright software
 * bitmap no longer than [OcrEngine.prepare]'s longest side, ready for Tesseract. Call off the
 * main thread: a 50 MP photo or a HEIC takes a moment to decode.
 *
 * ImageDecoder is tried first (applies the EXIF rotation itself, decodes HEIF); when it fails
 * the picture goes through BitmapFactory with the EXIF orientation applied by hand, so a picture
 * one decoder rejects still opens. The last failure's message is what the caller sees.
 */
object PhotoFiles {
    /** Photos longer than twice this on their longest side are decoded at a reduced size. */
    private const val TARGET_SIDE = 2000

    @Throws(IOException::class)
    fun load(context: Context, uri: Uri): Bitmap {
        val resolver = context.contentResolver
        val first = runCatching {
            ImageDecoder.decodeBitmap(ImageDecoder.createSource(resolver, uri)) { decoder, info, _ ->
                decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                decoder.isMutableRequired = false
                val longest = maxOf(info.size.width, info.size.height)
                if (longest > 2 * TARGET_SIDE) decoder.setTargetSampleSize(longest / TARGET_SIDE)
            }
        }
        first.getOrNull()?.let { return OcrEngine.prepare(it) }

        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
            ?: throw IOException("cannot open $uri")
        val longest = maxOf(bounds.outWidth, bounds.outHeight)
        val opts = BitmapFactory.Options().apply {
            inSampleSize = if (longest > 2 * TARGET_SIDE) longest / TARGET_SIDE else 1
            inPreferredConfig = Bitmap.Config.ARGB_8888
        }
        val bitmap = resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, opts) }
            ?: throw (first.exceptionOrNull() as? IOException ?: IOException(first.exceptionOrNull()?.message ?: "cannot decode $uri"))
        val rotation = runCatching {
            resolver.openInputStream(uri)?.use { input ->
                when (ExifInterface(input).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)) {
                    ExifInterface.ORIENTATION_ROTATE_90 -> 90
                    ExifInterface.ORIENTATION_ROTATE_180 -> 180
                    ExifInterface.ORIENTATION_ROTATE_270 -> 270
                    else -> 0
                }
            }
        }.getOrNull() ?: 0
        return OcrEngine.prepare(bitmap, rotation)
    }
}
