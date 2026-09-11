package com.offlinetranslator.app

import android.content.Context
import com.offlinetranslator.TranslationBackend

/** Everything the app remembers between launches. Small enough for SharedPreferences. */
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("interpreter", Context.MODE_PRIVATE)

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

    /** Manifest id of the LLM to use; null means the manifest's default. */
    var llmId: String?
        get() = sp.getString("llmId", null)
        set(v) = sp.edit().putString("llmId", v).apply()

    /** True once the user has finished the first-run download at least once. */
    var setupDone: Boolean
        get() = sp.getBoolean("setupDone", false)
        set(v) = sp.edit().putBoolean("setupDone", v).apply()

    /** Ask before downloading over a metered connection. */
    var wifiOnly: Boolean
        get() = sp.getBoolean("wifiOnly", true)
        set(v) = sp.edit().putBoolean("wifiOnly", v).apply()
}
