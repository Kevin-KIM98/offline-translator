plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Version of the engine release this app links against.
val engineVersion = "0.3.5"
val useLocalEngine = providers.gradleProperty("useLocalEngine").orNull == "true"
val engineAar = layout.projectDirectory.file("libs/offline-translator-$engineVersion.aar").asFile

// Fetches the prebuilt AAR on first build so a clone builds with no manual steps.
val downloadEngineAar by tasks.registering {
    description = "Downloads offline-translator-$engineVersion.aar from the GitHub release."
    outputs.file(engineAar)
    onlyIf { !useLocalEngine }
    doLast {
        if (engineAar.exists() && engineAar.length() > 0) return@doLast
        val url = "https://github.com/Kevin-KIM98/offline-translator/releases/download/" +
            "v$engineVersion/offline-translator-$engineVersion.aar"
        logger.lifecycle("Downloading engine: $url")
        engineAar.parentFile.mkdirs()
        val tmp = File(engineAar.path + ".part")
        uri(url).toURL().openStream().use { input -> tmp.outputStream().use { input.copyTo(it) } }
        if (tmp.length() < 1_000_000) throw GradleException("engine AAR download looks truncated (${tmp.length()} bytes)")
        tmp.renameTo(engineAar)
    }
}

android {
    namespace = "com.offlinetranslator.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.offlinetranslator.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"
        // The engine ships arm64-v8a; add x86_64 only when building the engine from source.
        ndk { abiFilters += if (useLocalEngine) listOf("arm64-v8a", "x86_64") else listOf("arm64-v8a") }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Unsigned by default; CI signs with a debug key so the APK is installable.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    buildFeatures { compose = true }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    packaging {
        jniLibs.useLegacyPackaging = false
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
}

dependencies {
    if (useLocalEngine) {
        implementation(project(":engine"))
    } else {
        implementation(files(engineAar).builtBy(downloadEngineAar))
    }

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.6")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    val composeBom = platform("androidx.compose:compose-bom:2024.09.02")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    debugImplementation("androidx.compose.ui:ui-tooling")
}
