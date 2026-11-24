package com.rollkey.rollinglock

import com.google.firebase.messaging.FirebaseMessagingService
import com.google.firebase.messaging.RemoteMessage

/**
 * Receives the lock's push alerts.
 *
 * App closed / in background: Android shows the notification by itself (the
 * message carries a "notification" block and the lock_alerts channel); this
 * class is not called. Tapping it opens the app, which records it from the
 * intent extras (see MainActivity).
 *
 * App open: this class is called, records the alert and shows it.
 */
class LockMessagingService : FirebaseMessagingService() {

    override fun onMessageReceived(message: RemoteMessage) {
        val event = message.data["event"] ?: "unknown"
        val title = message.notification?.title ?: "RollKey"
        val body = message.notification?.body ?: Alerts.label(event)
        val id = message.messageId ?: "${message.sentTime}-$event"
        Alerts.record(applicationContext, id, event, body, message.sentTime.takeIf { it > 0 } ?: System.currentTimeMillis())
        Alerts.show(applicationContext, id, title, body)
    }

    override fun onNewToken(token: String) {
        // Topic subscriptions follow the app instance; re-subscribe to be safe.
        // The token itself is never logged or shown.
        Alerts.refreshSetup(applicationContext)
    }
}
