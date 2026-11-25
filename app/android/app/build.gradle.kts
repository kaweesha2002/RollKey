plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Firebase config. google-services.json is git-ignored: download it from the
// Firebase console (Project settings -> Your apps -> Android) into this folder.
// Without it the app still builds, but push alerts are off.
val hasFirebaseConfig = file("google-services.json").exists()
if (hasFirebaseConfig) apply(plugin = "com.google.gms.google-services")

android {
    namespace = "com.rollkey.rollinglock"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.rollkey.rollinglock"
        minSdk = 26
        targetSdk = 37
        versionCode = 3
        versionName = "2.1"
        buildConfigField("boolean", "HAS_FIREBASE_CONFIG", "$hasFirebaseConfig")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.fragment.ktx)
    implementation(libs.androidx.biometric)
    implementation(platform(libs.firebase.bom))
    implementation(libs.firebase.messaging)
}
