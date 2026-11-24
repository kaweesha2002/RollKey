package com.rollkey.rollinglock

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.WindowManager
import android.widget.Toast
import androidx.fragment.app.FragmentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.TextButton
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat

private val Bg      = Color(0xFF0B1220)
private val Card    = Color(0xFF131C2E)
private val Accent  = Color(0xFF4F9DFF)
private val Good    = Color(0xFF3DDC97)
private val Warn    = Color(0xFFFFC857)
private val Bad     = Color(0xFFFF6B6B)
private val Muted   = Color(0xFF8A97AD)
private val TextHi  = Color(0xFFEAF0FA)

// true = no screenshots / hidden in the recent-apps view. Set to false while testing
// if you want to take screenshots of the app.
private const val BLOCK_SCREENSHOTS = true

// The app locks again after being in the background longer than this.
private const val AUTO_LOCK_MS = 30_000L

class MainActivity : FragmentActivity() {

    private lateinit var ble: LockBle
    private var backgroundedAt = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (BLOCK_SCREENSHOTS) {
            window.setFlags(
                WindowManager.LayoutParams.FLAG_SECURE,
                WindowManager.LayoutParams.FLAG_SECURE
            )
        }
        ble = LockBle(applicationContext)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme(primary = Accent, background = Bg)) {
                AppRoot(ble)
            }
        }
    }

    override fun onStop() {
        backgroundedAt = SystemClock.elapsedRealtime()
        super.onStop()
    }

    override fun onStart() {
        super.onStart()
        if (AppLockState.unlocked && backgroundedAt > 0 &&
            SystemClock.elapsedRealtime() - backgroundedAt > AUTO_LOCK_MS
        ) {
            ble.reset()
            AppLockState.unlocked = false
        }
    }

    override fun onDestroy() {
        ble.release()
        super.onDestroy()
    }
}

@Composable
fun AppRoot(ble: LockBle) {
    val context = LocalContext.current
    val store = remember { PinStore(context) }
    var pinSet by remember { mutableStateOf(store.isSet()) }
    var changeStage by remember { mutableStateOf(0) }   // 0 none, 1 verify current, 2 choose new
    val unlocked = AppLockState.unlocked

    val activity = remember { context.findFragmentActivity() }
    val bioAvailable = remember { Biometric.available(context) }
    var bioEnabled by remember { mutableStateOf(store.biometricEnabled()) }

    // Returns a function that shows the fingerprint prompt, or null if not usable.
    fun bioFor(onOk: () -> Unit): (() -> Unit)? =
        if (activity != null && bioAvailable && bioEnabled) {
            { Biometric.prompt(activity, "Unlock", "Use your fingerprint to open the app", onOk) }
        } else null

    when {
        !pinSet -> PinScreen(
            title = "Create a PIN",
            subtitle = "Choose a 6-digit PIN to protect this app",
            mode = PinMode.CREATE,
            store = store,
            onSuccess = { pin ->
                store.set(pin)
                pinSet = true
                AppLockState.unlocked = true
            }
        )
        !unlocked -> PinScreen(
            title = "Enter PIN",
            subtitle = "Unlock to view the lock code",
            mode = PinMode.UNLOCK,
            store = store,
            onSuccess = { AppLockState.unlocked = true },
            onBiometric = bioFor { AppLockState.unlocked = true }
        )
        changeStage == 1 -> PinScreen(
            title = "Current PIN",
            subtitle = "Enter your current PIN",
            mode = PinMode.UNLOCK,
            store = store,
            onSuccess = { changeStage = 2 },
            onCancel = { changeStage = 0 },
            onBiometric = bioFor { changeStage = 2 }
        )
        changeStage == 2 -> PinScreen(
            title = "New PIN",
            subtitle = "Choose a new 6-digit PIN",
            mode = PinMode.CREATE,
            store = store,
            onSuccess = { pin ->
                store.set(pin)
                changeStage = 0
            },
            onCancel = { changeStage = 0 }
        )
        else -> LockScreen(
            ble = ble,
            onLockNow = {
                ble.reset()
                AppLockState.unlocked = false
            },
            onChangePin = { changeStage = 1 },
            bioAvailable = bioAvailable,
            bioEnabled = bioEnabled,
            onToggleBio = {
                if (bioEnabled) {
                    store.setBiometricEnabled(false)
                    bioEnabled = false
                } else if (activity != null) {
                    Biometric.prompt(
                        activity,
                        "Turn on fingerprint unlock",
                        "Confirm with your fingerprint"
                    ) {
                        store.setBiometricEnabled(true)
                        bioEnabled = true
                    }
                }
            }
        )
    }
}

@Composable
fun LockScreen(
    ble: LockBle,
    onLockNow: () -> Unit,
    onChangePin: () -> Unit,
    bioAvailable: Boolean,
    bioEnabled: Boolean,
    onToggleBio: () -> Unit
) {
    val ui by ble.state.collectAsState()
    val context = LocalContext.current

    val perms = remember {
        if (Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) ble.start() else ble.permissionDenied()
    }

    fun begin() {
        val granted = perms.all {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        }
        if (granted) ble.start() else launcher.launch(perms)
    }

    val (statusText, statusColor) = when (ui.status) {
        LockStatus.IDLE          -> "Not connected" to Muted
        LockStatus.BT_OFF        -> "Bluetooth is off" to Bad
        LockStatus.NO_PERMISSION -> "Permission needed" to Bad
        LockStatus.SCANNING      -> "Searching for lock…" to Warn
        LockStatus.CONNECTING    -> "Connecting…" to Warn
        LockStatus.PAIRING       -> "Pairing – enter the passkey" to Warn
        LockStatus.PAIRING_FAILED-> "Pairing failed" to Bad
        LockStatus.CONNECTED     -> "Connected" to Good
        LockStatus.DISCONNECTED  -> "Disconnected" to Muted
        LockStatus.NOT_FOUND     -> "Lock not found" to Bad
    }

    val busy = ui.status == LockStatus.SCANNING ||
        ui.status == LockStatus.CONNECTING ||
        ui.status == LockStatus.PAIRING
    val connected = ui.status == LockStatus.CONNECTED
    val hasCode = ui.code.isNotEmpty() && ui.code.all { it.isDigit() }
    val digits = ui.code.take(4).padEnd(4, '-')

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Bg)
            .systemBarsPadding()
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(16.dp))
        Text(
            "ROLLING LOCK",
            color = Accent,
            fontSize = 14.sp,
            fontWeight = FontWeight.SemiBold,
            letterSpacing = 4.sp
        )
        Spacer(Modifier.height(24.dp))

        // Status pill
        Row(
            modifier = Modifier
                .clip(RoundedCornerShape(50))
                .background(Card)
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(Modifier.size(10.dp).clip(CircleShape).background(statusColor))
            Spacer(Modifier.size(10.dp))
            Text(statusText, color = TextHi, fontSize = 14.sp)
        }

        Spacer(Modifier.weight(1f))

        // Code card
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(28.dp))
                .background(Card)
                .padding(vertical = 32.dp, horizontal = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text("NEXT CODE", color = Muted, fontSize = 12.sp, letterSpacing = 3.sp)
            Spacer(Modifier.height(20.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                digits.forEach { ch ->
                    Box(
                        modifier = Modifier
                            .size(width = 62.dp, height = 88.dp)
                            .clip(RoundedCornerShape(16.dp))
                            .background(Bg),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            ch.toString(),
                            color = if (hasCode) Accent else Muted,
                            fontSize = 44.sp,
                            fontWeight = FontWeight.Bold,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
            Text(
                if (ui.updatedAt != null) "Updated ${ui.updatedAt}" else "Tap Scan & connect to view the code",
                color = Muted,
                fontSize = 13.sp
            )
        }

        Spacer(Modifier.weight(1f))

        OutlinedButton(
            onClick = {
                val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("Lock code", ui.code))
                Toast.makeText(context, "Code copied", Toast.LENGTH_SHORT).show()
            },
            enabled = hasCode,
            modifier = Modifier.fillMaxWidth().height(52.dp),
            shape = RoundedCornerShape(16.dp)
        ) { Text("Copy code") }

        Spacer(Modifier.height(12.dp))

        Button(
            onClick = { if (busy || connected) ble.stop() else begin() },
            modifier = Modifier.fillMaxWidth().height(56.dp),
            shape = RoundedCornerShape(16.dp),
            colors = ButtonDefaults.buttonColors(containerColor = Accent, contentColor = Bg)
        ) {
            Text(
                when {
                    busy      -> "Cancel"
                    connected -> "Disconnect"
                    else      -> "Scan & connect"
                },
                fontWeight = FontWeight.SemiBold,
                fontSize = 16.sp
            )
        }

        Spacer(Modifier.height(12.dp))
        Text(
            "Tap Scan & connect to view the lock's current code. The first time, Android asks for the lock's 6-digit passkey. Stay connected and the code updates by itself after each unlock.",
            color = Muted,
            fontSize = 12.sp,
            textAlign = TextAlign.Center
        )

        // App options at the very bottom
        Spacer(Modifier.height(12.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly
        ) {
            TextButton(onClick = onLockNow) {
                Text("Lock app", color = Warn, fontSize = 14.sp, fontWeight = FontWeight.Medium)
            }
            TextButton(onClick = onChangePin) {
                Text("Change PIN", color = Warn, fontSize = 14.sp, fontWeight = FontWeight.Medium)
            }
        }
        if (bioAvailable) {
            TextButton(onClick = onToggleBio) {
                Text(
                    if (bioEnabled) "Fingerprint unlock: On" else "Fingerprint unlock: Off",
                    color = Muted,
                    fontSize = 13.sp
                )
            }
        }
    }
}
