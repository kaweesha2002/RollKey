package com.rollkey.rollinglock

import android.content.Context
import android.util.Base64
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.PBEKeySpec

/** In-memory "is the app unlocked right now" flag. Resets when the app process dies. */
object AppLockState {
    var unlocked by mutableStateOf(false)
}

/**
 * Stores the app PIN as a salted PBKDF2 hash (never the PIN itself) and
 * remembers failed attempts so guessing gets slower after 5 wrong tries.
 */
class PinStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("app_lock", Context.MODE_PRIVATE)

    fun isSet(): Boolean = prefs.contains("hash")

    fun set(pin: String) {
        val salt = ByteArray(16).also { SecureRandom().nextBytes(it) }
        prefs.edit()
            .putString("salt", Base64.encodeToString(salt, Base64.NO_WRAP))
            .putString("hash", Base64.encodeToString(hash(pin, salt), Base64.NO_WRAP))
            .putInt("fails", 0)
            .putLong("until", 0L)
            .apply()
    }

    fun verify(pin: String): Boolean {
        val salt = Base64.decode(prefs.getString("salt", null) ?: return false, Base64.NO_WRAP)
        val want = Base64.decode(prefs.getString("hash", null) ?: return false, Base64.NO_WRAP)
        return MessageDigest.isEqual(want, hash(pin, salt))
    }

    fun biometricEnabled(): Boolean = prefs.getBoolean("bio", false)

    fun setBiometricEnabled(on: Boolean) {
        prefs.edit().putBoolean("bio", on).apply()
    }

    /** Wall-clock time (ms) until which entry is blocked; 0 if not blocked. */
    fun lockedUntil(): Long = prefs.getLong("until", 0L)

    fun registerFail() {
        val fails = prefs.getInt("fails", 0) + 1
        var until = 0L
        if (fails >= 5) {
            // 30 s, 60 s, 120 s ... capped at 15 minutes
            val seconds = minOf(30L * (1L shl minOf(fails - 5, 5)), 900L)
            until = System.currentTimeMillis() + seconds * 1000L
        }
        prefs.edit().putInt("fails", fails).putLong("until", until).apply()
    }

    fun resetFails() {
        prefs.edit().putInt("fails", 0).putLong("until", 0L).apply()
    }

    private fun hash(pin: String, salt: ByteArray): ByteArray {
        val spec = PBEKeySpec(pin.toCharArray(), salt, 60_000, 256)
        return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
    }
}
