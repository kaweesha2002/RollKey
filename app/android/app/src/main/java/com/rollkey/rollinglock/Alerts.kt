package com.rollkey.rollinglock

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.google.firebase.FirebaseApp
import com.google.firebase.messaging.FirebaseMessaging
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.json.JSONArray
import org.json.JSONObject

/** One received push alert. Only the event type and time: the lock never sends secrets. */
data class AlertItem(val id: String, val event: String, val text: String, val time: Long)

enum class AlertSetup {
    NO_FIREBASE_CONFIG,   // google-services.json missing at build time
    NO_TOPIC,             // secrets.properties / ALERT_TOPIC missing at build time
    NO_PERMISSION,        // notifications blocked on this phone
    SUBSCRIBING,
    SUBSCRIBE_FAILED,
    READY
}

object Alerts {
    const val CHANNEL_ID = "lock_alerts"
    private const val MAX_ITEMS = 30

    private val _items = MutableStateFlow<List<AlertItem>>(emptyList())
    val items: StateFlow<List<AlertItem>> = _items.asStateFlow()

    private val _setup = MutableStateFlow(AlertSetup.SUBSCRIBING)
    val setup: StateFlow<AlertSetup> = _setup.asStateFlow()

    fun label(event: String): String = when (event) {
        "test"         -> "Test alert from the lock"
        "wrong_code"   -> "Wrong code entered"
        "lockout"      -> "Lock blocked after wrong codes"
        "hard_lockout" -> "Lock fully locked out"
        "unlocked"     -> "Safe unlocked"
        "wipe_attempt" -> "Wipe attempted"
        "wipe_done"    -> "Lock was wiped"
        else           -> "Lock alert"
    }

    fun isUrgent(event: String) =
        event == "wrong_code" || event == "lockout" || event == "hard_lockout" ||
            event == "wipe_attempt" || event == "wipe_done"

    // ---------------- Setup ----------------

    fun createChannel(context: Context) {
        val ch = NotificationChannel(
            CHANNEL_ID,
            context.getString(R.string.alert_channel_name),
            NotificationManager.IMPORTANCE_HIGH
        ).apply { description = context.getString(R.string.alert_channel_desc) }
        context.getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
    }

    fun notificationsAllowed(context: Context): Boolean =
        Build.VERSION.SDK_INT < 33 ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED

    /** Checks config + permission, then subscribes to the lock's topic. */
    fun refreshSetup(context: Context) {
        val app = context.applicationContext
        when {
            !BuildConfig.HAS_FIREBASE_CONFIG || FirebaseApp.getApps(app).isEmpty() ->
                _setup.value = AlertSetup.NO_FIREBASE_CONFIG
            BuildConfig.ALERT_TOPIC.isBlank() -> _setup.value = AlertSetup.NO_TOPIC
            !notificationsAllowed(app) -> _setup.value = AlertSetup.NO_PERMISSION
            else -> {
                _setup.value = AlertSetup.SUBSCRIBING
                FirebaseMessaging.getInstance().subscribeToTopic(BuildConfig.ALERT_TOPIC)
                    .addOnCompleteListener { task ->
                        _setup.value = if (task.isSuccessful) AlertSetup.READY else AlertSetup.SUBSCRIBE_FAILED
                    }
            }
        }
    }

    // ---------------- History (local only) ----------------

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences("alert_history", Context.MODE_PRIVATE)

    fun load(context: Context) {
        val arr = JSONArray(prefs(context).getString("items", "[]"))
        _items.value = (0 until arr.length()).map { i ->
            val o = arr.getJSONObject(i)
            AlertItem(o.getString("id"), o.getString("event"), o.getString("text"), o.getLong("time"))
        }
    }

    /** Adds an alert unless one with the same message id is already stored. */
    fun record(context: Context, id: String, event: String, text: String?, time: Long) {
        if (_items.value.isEmpty()) load(context)
        if (_items.value.any { it.id == id }) return
        val list = (listOf(AlertItem(id, event, text ?: label(event), time)) + _items.value).take(MAX_ITEMS)
        _items.value = list
        val arr = JSONArray()
        list.forEach {
            arr.put(JSONObject().put("id", it.id).put("event", it.event).put("text", it.text).put("time", it.time))
        }
        prefs(context).edit().putString("items", arr.toString()).apply()
    }

    fun clear(context: Context) {
        _items.value = emptyList()
        prefs(context).edit().remove("items").apply()
    }

    // ---------------- Showing a notification (app in foreground) ----------------

    fun show(context: Context, id: String, title: String, body: String) {
        if (!notificationsAllowed(context)) return
        val open = PendingIntent.getActivity(
            context, 0,
            Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val n = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_lock)
            .setColor(ContextCompat.getColor(context, R.color.alert_accent))
            .setContentTitle(title)
            .setContentText(body)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setAutoCancel(true)
            .setContentIntent(open)
            .build()
        try {
            NotificationManagerCompat.from(context).notify(id.hashCode(), n)
        } catch (_: SecurityException) {
            // permission revoked between the check and notify()
        }
    }
}
