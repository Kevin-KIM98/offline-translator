package com.offlinetranslator.app

/**
 * Which speech model and which LLM the two conversation languages call for, when the user leaves
 * the choice to the app ("automatic" in Settings). The rule is the measured trade-off between
 * accuracy and time, per language:
 *
 *  - Whisper small hears Korean, English, Spanish and Russian as well as medium does (CER 0.0,
 *    0.0, 0.1, 0.3 % against medium's ru 0.4 %) at a third of the time, so it is the choice for
 *    those. Medium is the choice when a language of the pair loses more than 1 % CER with small:
 *    Thai (14.5 → 9.1 %), Vietnamese (3.9 → 0.8 %), French (3.7 → 0.0 %), Japanese (2.1 → 0.0 %),
 *    Chinese (2.7 → 0.4 %) and Indonesian (3.5 → 2.4 %), which is [MEDIUM_LANGS]. On phones with
 *    less than 7.5 GB of memory small is used regardless.
 *  - The LLM only translates into Thai. Qwen2.5 3B gets 31 of 40 Korean → Thai sentences right
 *    against the 1.5B's 26, at about twice the time, so a pair with Thai gets the 3B on phones
 *    with 7.5 GB or more. Any other pair never uses it and keeps the small one, which loads
 *    fastest, as the fallback for a missing translation pair.
 */
object ModelPolicy {
    const val STT_SMALL = "whisper-small-q5_1"
    const val STT_MEDIUM = "whisper-medium-q5_0"
    const val LLM_SMALL = "qwen2.5-1.5b-instruct-q4_k_m"
    const val LLM_LARGE = "qwen2.5-3b-instruct-q4_k_m"

    /** Languages whisper small measurably misses on (synthetic clips, `tests/eval/run_stt_eval.py`). */
    val MEDIUM_LANGS = setOf("th", "vi", "fr", "ja", "zh", "id")

    /** Speech model for [langs]; [roomy] is 7.5 GB of memory or more. */
    fun sttFor(langs: Collection<String>, roomy: Boolean): String =
        if (roomy && langs.any { it in MEDIUM_LANGS }) STT_MEDIUM else STT_SMALL

    /** LLM for [langs]. */
    fun llmFor(langs: Collection<String>, roomy: Boolean): String =
        if (roomy && "th" in langs) LLM_LARGE else LLM_SMALL

    /** The language that made [sttFor] pick medium, for the settings screen; null for small. */
    fun sttReason(langs: Collection<String>, roomy: Boolean): String? =
        if (roomy) langs.firstOrNull { it in MEDIUM_LANGS } else null
}
