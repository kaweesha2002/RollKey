package com.rollkey.rollinglock

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay

enum class PinMode { CREATE, UNLOCK }

const val PIN_LEN = 6

private val PBg     = Color(0xFF0B1220)
private val PCard   = Color(0xFF131C2E)
private val PAccent = Color(0xFF4F9DFF)
private val PBad    = Color(0xFFFF6B6B)
private val PMuted  = Color(0xFF8A97AD)
private val PText   = Color(0xFFEAF0FA)

/**
 * Full-screen PIN pad.
 * CREATE: type a new PIN twice. UNLOCK: type the saved PIN.
 */
@Composable
fun PinScreen(
    title: String,
    subtitle: String,
    mode: PinMode,
    store: PinStore,
    onSuccess: (String) -> Unit,
    onCancel: (() -> Unit)? = null,
    onBiometric: (() -> Unit)? = null
) {
    var entry by remember { mutableStateOf("") }
    var first by remember { mutableStateOf<String?>(null) }
    var message by remember { mutableStateOf("") }
    var now by remember { mutableStateOf(System.currentTimeMillis()) }

    // Ask for the fingerprint straight away when it is available.
    LaunchedEffect(Unit) {
        if (mode == PinMode.UNLOCK) onBiometric?.invoke()
    }

    LaunchedEffect(Unit) {
        while (true) {
            now = System.currentTimeMillis()
            delay(500)
        }
    }

    val lockedSecs = ((store.lockedUntil() - now + 999) / 1000).coerceAtLeast(0)
    val locked = mode == PinMode.UNLOCK && lockedSecs > 0

    // Set when the last digit is typed. The PIN is checked a moment later so the
    // 6th dot is visibly filled before anything changes on screen.
    var pending by remember { mutableStateOf<String?>(null) }

    fun process(pin: String) {
        when (mode) {
            PinMode.UNLOCK -> {
                if (store.verify(pin)) {
                    store.resetFails()
                    onSuccess(pin)
                } else {
                    store.registerFail()
                    message = "Wrong PIN"
                    entry = ""
                }
            }
            PinMode.CREATE -> {
                val f = first
                if (f == null) {
                    first = pin
                    entry = ""
                    message = "Enter it again to confirm"
                } else if (f == pin) {
                    onSuccess(pin)
                } else {
                    first = null
                    entry = ""
                    message = "PINs did not match. Start again."
                }
            }
        }
    }

    LaunchedEffect(pending) {
        val pin = pending ?: return@LaunchedEffect
        delay(200)
        process(pin)
        pending = null
    }

    fun press(d: Char) {
        if (locked || pending != null || entry.length >= PIN_LEN) return
        entry += d
        if (entry.length == PIN_LEN) pending = entry
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(PBg)
            .systemBarsPadding()
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            "ROLLING LOCK",
            color = PAccent,
            fontSize = 14.sp,
            fontWeight = FontWeight.SemiBold,
            letterSpacing = 4.sp
        )
        Spacer(Modifier.height(28.dp))
        Text(title, color = PText, fontSize = 22.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(6.dp))
        Text(subtitle, color = PMuted, fontSize = 13.sp, textAlign = TextAlign.Center)
        Spacer(Modifier.height(24.dp))

        Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            repeat(PIN_LEN) { i ->
                Box(
                    Modifier
                        .size(14.dp)
                        .clip(CircleShape)
                        .background(if (i < entry.length) PAccent else PCard)
                )
            }
        }

        Spacer(Modifier.height(8.dp))
        Box(
            modifier = Modifier.height(32.dp),
            contentAlignment = Alignment.TopCenter
        ) {
            Text(
                when {
                    locked -> "Too many attempts. Try again in ${lockedSecs}s"
                    else -> message
                },
                color = if (locked || message.startsWith("Wrong") || message.startsWith("PINs")) PBad else PMuted,
                fontSize = 13.sp,
                lineHeight = 18.sp,
                textAlign = TextAlign.Center
            )
        }
        Spacer(Modifier.height(12.dp))

        listOf("123", "456", "789").forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(20.dp)) {
                row.forEach { c -> PinKey(c.toString()) { press(c) } }
            }
            Spacer(Modifier.height(16.dp))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(20.dp)) {
            if (onCancel != null) PinTextKey("Cancel") { onCancel() } else PinTextKey("") {}
            PinKey("0") { press('0') }
            PinTextKey("\u232B") { if (entry.isNotEmpty()) entry = entry.dropLast(1) }
        }
        if (onBiometric != null) {
            Spacer(Modifier.height(8.dp))
            TextButton(onClick = onBiometric) {
                Text("Use fingerprint", color = PAccent, fontSize = 14.sp)
            }
        }
    }
}

@Composable
private fun PinKey(label: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .size(72.dp)
            .clip(CircleShape)
            .background(PCard)
            .clickable { onClick() },
        contentAlignment = Alignment.Center
    ) {
        Text(label, color = PText, fontSize = 26.sp, fontWeight = FontWeight.Medium)
    }
}

@Composable
private fun PinTextKey(label: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .size(72.dp)
            .clip(CircleShape)
            .clickable(enabled = label.isNotEmpty()) { onClick() },
        contentAlignment = Alignment.Center
    ) {
        Text(label, color = PMuted, fontSize = if (label.length > 1) 14.sp else 24.sp)
    }
}
