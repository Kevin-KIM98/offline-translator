package com.offlinetranslator.app

import android.content.Context
import com.offlinetranslator.ModelStatus

/**
 * Human names for manifest entries. Shared by the setup screen, the settings screen and the
 * download progress, so a model is called the same thing everywhere.
 */
fun modelTitle(context: Context, m: ModelStatus): String = when (m.kind) {
    "stt" -> if (m.label.isBlank()) context.getString(R.string.model_stt) else context.getString(R.string.model_stt_named, m.label)
    "llm" -> m.label.ifBlank { context.getString(R.string.model_llm) }
    "nmt" -> {
        val parts = m.pair.split("-")
        if (parts.size == 2) {
            context.getString(R.string.model_nmt_pair, Lang.of(parts[0]).name, Lang.of(parts[1]).name)
        } else {
            context.getString(R.string.model_nmt_generic, m.pair)
        }
    }
    else -> m.id
}

fun modelSubtitle(context: Context, m: ModelStatus): String = when (m.kind) {
    "stt" -> context.getString(R.string.model_stt_sub)
    "llm" -> context.getString(R.string.model_llm_sub)
    "nmt" -> context.getString(R.string.model_nmt_sub)
    else -> m.kind
}
