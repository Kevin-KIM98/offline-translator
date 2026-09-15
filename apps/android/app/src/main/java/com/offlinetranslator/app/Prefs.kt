package com.offlinetranslator.app

import android.app.ActivityManager
import android.content.Context
import com.offlinetranslator.TranslationBackend

/** Everything the app remembers between launches. Small enough for SharedPreferences. */
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("interpreter", Context.MODE_PRIVATE)

    // Model defaults follow the phone. With 8 GB of memory or more the accurate models are the
    // default (whisper medium: th 14.5% -> 9.1% CER, vi 3.9% -> 0.8%; Qwen2.5 3B: 26 -> 31 of 40
    // Korean->Thai sentences right), at two to three times the time per sentence. Smaller phones
    // start with the fast models. Either way the user can change them in Settings.
    val roomy: Boolean = totalMemoryBytes(context) >= 7_500_000_000L
    val defaultSttId: String = if (roomy) ModelPolicy.STT_MEDIUM else ModelPolicy.STT_SMALL
    val defaultLlmId: String = if (roomy) ModelPolicy.LLM_LARGE else ModelPolicy.LLM_SMALL

    /**
     * The app picks the speech model and the LLM from the conversation languages ([ModelPolicy])
     * unless the user chose one in Settings; then [sttId] / [llmId] is that choice.
     */
    var sttAuto: Boolean
        get() = sp.getBoolean("sttAuto", true)
        set(v) = sp.edit().putBoolean("sttAuto", v).apply()

    var llmAuto: Boolean
        get() = sp.getBoolean("llmAuto", true)
        set(v) = sp.edit().putBoolean("llmAuto", v).apply()

    var langA: String
        get() = sp.getString("langA", "ko")!!
        set(v) = sp.edit().putString("langA", v).apply()

    var langB: String
        get() = sp.getString("langB", "en")!!
        set(v) = sp.edit().putString("langB", v).apply()

    var speak: Boolean
        get() = sp.getBoolean("speak", true)
        set(v) = sp.edit().putBoolean("speak", v).apply()

    var speechRate: Float
        get() = sp.getFloat("speechRate", 1.0f)
        set(v) = sp.edit().putFloat("speechRate", v).apply()

    var backend: TranslationBackend
        get() = runCatching { TranslationBackend.valueOf(sp.getString("backend", "AUTO")!!) }
            .getOrDefault(TranslationBackend.AUTO)
        set(v) = sp.edit().putString("backend", v.name).apply()

    /** Manifest id of the LLM to use; unset means the phone's default (see [defaultLlmId]). */
    var llmId: String?
        get() = sp.getString("llmId", null) ?: defaultLlmId
        set(v) = sp.edit().putString("llmId", v).apply()

    /** Whisper on the GPU (Vulkan), experimental; off by default. */
    var useGpu: Boolean
        get() = sp.getBoolean("useGpu", false)
        set(v) = sp.edit().putBoolean("useGpu", v).apply()

    /**
     * True from the moment the engine is opened with the GPU until that succeeded. Still true at
     * the next launch means the app died inside the GPU driver: the switch is turned off again.
     */
    var gpuTrialPending: Boolean
        get() = sp.getBoolean("gpuTrialPending", false)
        set(v) = sp.edit().putBoolean("gpuTrialPending", v).apply()

    /** Manifest id of the speech (whisper) model to use; unset means the phone's default (see [defaultSttId]). */
    var sttId: String?
        get() = sp.getString("sttId", null) ?: defaultSttId
        set(v) = sp.edit().putString("sttId", v).apply()

    /** True once the user has finished the first-run download at least once. */
    var setupDone: Boolean
        get() = sp.getBoolean("setupDone", false)
        set(v) = sp.edit().putBoolean("setupDone", v).apply()

    /** Ask before downloading over a metered connection. */
    var wifiOnly: Boolean
        get() = sp.getBoolean("wifiOnly", true)
        set(v) = sp.edit().putBoolean("wifiOnly", v).apply()
}

/** Physical memory of the phone, 0 when unknown. */
fun totalMemoryBytes(context: Context): Long {
    val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return 0L
    val info = ActivityManager.MemoryInfo()
    am.getMemoryInfo(info)
    return info.totalMem
}
