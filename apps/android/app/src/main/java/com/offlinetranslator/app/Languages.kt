package com.offlinetranslator.app

/** One of the seven languages the engine supports, as shown in the UI. */
data class Lang(
    val code: String,
    /** Endonym — what speakers of the language call it. */
    val name: String,
    /** Short label for the talk buttons when space is tight. */
    val short: String,
) {
    companion object {
        val ALL = listOf(
            Lang("ko", "한국어", "한국어"),
            Lang("en", "English", "English"),
            Lang("es", "Español", "Español"),
            Lang("ja", "日本語", "日本語"),
            Lang("zh", "中文", "中文"),
            Lang("vi", "Tiếng Việt", "Tiếng Việt"),
            Lang("th", "ไทย", "ไทย"),
        )

        fun of(code: String): Lang = ALL.firstOrNull { it.code == code } ?: Lang(code, code.uppercase(), code.uppercase())
    }
}

/** Which side of the conversation a turn belongs to. */
enum class Side { A, B }

fun Side.other(): Side = if (this == Side.A) Side.B else Side.A
