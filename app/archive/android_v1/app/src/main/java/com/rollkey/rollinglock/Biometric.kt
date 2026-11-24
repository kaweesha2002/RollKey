package com.rollkey.rollinglock

import android.content.Context
import android.content.ContextWrapper
import androidx.biometric.BiometricManager
import androidx.biometric.BiometricPrompt
import androidx.core.content.ContextCompat
import androidx.fragment.app.FragmentActivity

/** Fingerprint / secure face unlock helper (strong biometrics only). */
object Biometric {

    private const val AUTH = BiometricManager.Authenticators.BIOMETRIC_STRONG

    /** True if this phone has a strong biometric enrolled and ready. */
    fun available(context: Context): Boolean =
        BiometricManager.from(context).canAuthenticate(AUTH) == BiometricManager.BIOMETRIC_SUCCESS

    fun prompt(
        activity: FragmentActivity,
        title: String,
        subtitle: String,
        onSuccess: () -> Unit
    ) {
        val executor = ContextCompat.getMainExecutor(activity)
        val callback = object : BiometricPrompt.AuthenticationCallback() {
            override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
                onSuccess()
            }
            // Errors / cancel: do nothing, the PIN pad stays available.
        }
        val info = BiometricPrompt.PromptInfo.Builder()
            .setTitle(title)
            .setSubtitle(subtitle)
            .setNegativeButtonText("Use PIN")
            .setAllowedAuthenticators(AUTH)
            .build()
        BiometricPrompt(activity, executor, callback).authenticate(info)
    }
}

fun Context.findFragmentActivity(): FragmentActivity? {
    var c: Context = this
    while (c is ContextWrapper) {
        if (c is FragmentActivity) return c
        c = c.baseContext
    }
    return null
}
