package com.rollkey.rollinglock

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.provider.Settings
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
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
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.fragment.app.FragmentActivity
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

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
        Alerts.createChannel(this)
        Alerts.load(this)
        recordTappedAlert(intent)

        ble = LockBle(applicationContext)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme(primary = Accent, background = Bg)) {
                AppRoot(ble)
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        recordTappedAlert(intent)
    }

    /** An alert tapped while the app was closed arrives as intent extras. */
    private fun recordTappedAlert(intent: Intent?) {
        val extras = intent?.extras ?: return
        val event = extras.getString("event") ?: return
        val id = extras.getString("google.message_id") ?: return
        val sent = extras.getLong("google.sent_time", System.currentTimeMillis())
        Alerts.record(this, id, event, null, sent)
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

    // ---- Bluetooth permissions ----
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

    // ---- Notification permission (Android 13+) + topic subscription ----
    val notifLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { Alerts.refreshSetup(context) }
    fun askNotifications() {
        if (Build.VERSION.SDK_INT >= 33 && !Alerts.notificationsAllowed(context))
            notifLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        else Alerts.refreshSetup(context)
    }
    LaunchedEffect(Unit) { askNotifications() }

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
        LockStatus.REPAIR_NEEDED -> "Lock was reset – pair again" to Bad
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
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(8.dp))
        Text("ROLLKEY", color = Accent, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 4.sp)
        Spacer(Modifier.height(20.dp))

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

        if (ui.status == LockStatus.REPAIR_NEEDED) {
            Spacer(Modifier.height(16.dp))
            RepairCard()
        }

        Spacer(Modifier.height(24.dp))

        // Code card
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(28.dp))
                .background(Card)
                .padding(vertical = 28.dp, horizontal = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text("NEXT CODE", color = Muted, fontSize = 12.sp, letterSpacing = 3.sp)
            Spacer(Modifier.height(18.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                digits.forEach { ch ->
                    Box(
                        modifier = Modifier
                            .size(width = 62.dp, height = 84.dp)
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
            Spacer(Modifier.height(14.dp))
            Text(
                if (ui.updatedAt != null) "Updated ${ui.updatedAt}" else "Tap Scan & connect to view the code",
                color = Muted,
                fontSize = 13.sp
            )
        }

        Spacer(Modifier.height(16.dp))

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(
                onClick = {
                    val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    cm.setPrimaryClip(ClipData.newPlainText("Lock code", ui.code))
                    Toast.makeText(context, "Code copied", Toast.LENGTH_SHORT).show()
                },
                enabled = hasCode,
                modifier = Modifier.weight(1f).height(54.dp),
                shape = RoundedCornerShape(16.dp)
            ) { Text("Copy code") }

            Button(
                onClick = { if (busy || connected) ble.stop() else begin() },
                modifier = Modifier.weight(1f).height(54.dp),
                shape = RoundedCornerShape(16.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Accent, contentColor = Bg)
            ) {
                Text(
                    when {
                        busy      -> "Cancel"
                        connected -> "Disconnect"
                        else      -> "Scan & connect"
                    },
                    fontWeight = FontWeight.SemiBold
                )
            }
        }

        if (connected) {
            Spacer(Modifier.height(16.dp))
            WifiCard(ui, ble)
        }

        Spacer(Modifier.height(16.dp))
        AlertsCard(onAllowNotifications = { askNotifications() })

        Spacer(Modifier.height(12.dp))
        Text(
            "The first time, Android asks for the lock's 6-digit passkey. Stay connected and the code updates by itself after each unlock. Alerts arrive over the internet, even when you are away.",
            color = Muted,
            fontSize = 12.sp,
            textAlign = TextAlign.Center
        )

        Spacer(Modifier.height(8.dp))
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
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

/**
 * Shown when the phone still holds an old pairing that the lock forgot
 * (after a wipe). Android only lets the Settings app remove a pairing.
 */
@Composable
fun RepairCard() {
    val context = LocalContext.current
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(Card)
            .padding(18.dp)
    ) {
        Text("The lock was reset", color = Bad, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(8.dp))
        Text(
            "A wipe makes the lock forget every paired phone. This phone still has the old pairing, so it can't connect.\n\n" +
                "1. Open Bluetooth settings\n" +
                "2. Tap ⚙ next to RollingLock → Unpair\n" +
                "3. Come back, tap Scan & connect and enter the lock's new 6-digit key",
            color = TextHi,
            fontSize = 13.sp
        )
        Spacer(Modifier.height(12.dp))
        Button(
            onClick = {
                context.startActivity(
                    Intent(Settings.ACTION_BLUETOOTH_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            },
            modifier = Modifier.fillMaxWidth().height(48.dp),
            shape = RoundedCornerShape(14.dp),
            colors = ButtonDefaults.buttonColors(containerColor = Bad, contentColor = Bg)
        ) { Text("Open Bluetooth settings", fontWeight = FontWeight.SemiBold) }
    }
}

@Composable
fun AlertsCard(onAllowNotifications: () -> Unit) {
    val context = LocalContext.current
    val setup by Alerts.setup.collectAsState()
    val items by Alerts.items.collectAsState()
    val fmt = remember { SimpleDateFormat("dd MMM  HH:mm", Locale.getDefault()) }

    val (text, color) = when (setup) {
        AlertSetup.READY              -> "Push alerts on" to Good
        AlertSetup.SUBSCRIBING        -> "Setting up alerts…" to Warn
        AlertSetup.SUBSCRIBE_FAILED   -> "Could not subscribe – check internet" to Bad
        AlertSetup.NO_PERMISSION      -> "Notifications are blocked" to Bad
        AlertSetup.NO_FIREBASE_CONFIG -> "Alerts not set up in this build (google-services.json)" to Muted
        AlertSetup.NO_TOPIC           -> "Connect to your lock once to turn on alerts" to Muted
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(24.dp))
            .background(Card)
            .padding(18.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("ALERTS", color = Muted, fontSize = 12.sp, letterSpacing = 3.sp, modifier = Modifier.weight(1f))
            if (items.isNotEmpty()) {
                TextButton(onClick = { Alerts.clear(context) }) { Text("Clear", color = Muted, fontSize = 13.sp) }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(10.dp).clip(CircleShape).background(color))
            Spacer(Modifier.width(10.dp))
            Text(text, color = TextHi, fontSize = 14.sp)
        }
        if (setup == AlertSetup.NO_PERMISSION || setup == AlertSetup.SUBSCRIBE_FAILED) {
            TextButton(onClick = onAllowNotifications) {
                Text(if (setup == AlertSetup.NO_PERMISSION) "Allow notifications" else "Try again", color = Accent)
            }
        }

        Spacer(Modifier.height(10.dp))
        if (items.isEmpty()) {
            Text("No alerts yet", color = Muted, fontSize = 13.sp)
        } else {
            items.take(10).forEach { a ->
                Row(
                    modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Box(
                        Modifier.size(8.dp).clip(CircleShape)
                            .background(if (Alerts.isUrgent(a.event)) Bad else Good)
                    )
                    Spacer(Modifier.width(10.dp))
                    Text(a.text, color = TextHi, fontSize = 14.sp, modifier = Modifier.weight(1f))
                    Text(fmt.format(Date(a.time)), color = Muted, fontSize = 12.sp)
                }
            }
        }
    }
}

// ---------------- Lock Wi-Fi ----------------

private enum class WifiStage { STATUS, LIST, PASSWORD, CONNECTING }

private fun wifiText(status: String): Pair<String, Color> = when {
    status.startsWith("ok:")     -> "Connected to ${status.removePrefix("ok:")}" to Good
    status.startsWith("saved:")  -> "Connected to ${status.removePrefix("saved:")}" to Good
    status == "none"             -> "Wi-Fi not set up – alerts are off" to Warn
    status == "testing"          -> "Connecting…" to Warn
    status == "scanning"         -> "Searching for networks…" to Warn
    status == "fail:password"    -> "Wrong password" to Bad
    status == "fail:notfound"    -> "Network not found" to Bad
    status == "fail:timeout"     -> "No response from the network" to Bad
    status == "fail:busy"        -> "Lock is busy, try again" to Bad
    status == "fail:send"        -> "Could not send to the lock, try again" to Bad
    status == "fail:unsupported" -> "This lock's firmware has no Wi-Fi setup" to Muted
    status.startsWith("fail:")   -> "Could not connect" to Bad
    else                         -> "Checking Wi-Fi…" to Muted
}

/** 1..4 bars from RSSI (dBm). */
private fun signalBars(rssi: Int) = when {
    rssi >= -55 -> 4
    rssi >= -67 -> 3
    rssi >= -78 -> 2
    else        -> 1
}

@Composable
private fun SignalBars(rssi: Int) {
    val level = signalBars(rssi)
    Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(2.dp)) {
        for (i in 1..4) {
            Box(
                Modifier
                    .size(width = 4.dp, height = (3 + 3 * i).dp)
                    .clip(RoundedCornerShape(1.dp))
                    .background(if (i <= level) Accent else Muted.copy(alpha = 0.35f))
            )
        }
    }
}

@Composable
private fun SmallSpinner(text: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        CircularProgressIndicator(color = Accent, strokeWidth = 2.dp, modifier = Modifier.size(18.dp))
        Spacer(Modifier.width(10.dp))
        Text(text, color = Muted, fontSize = 13.sp)
    }
}

/** Shown while connected: lets the owner put the lock on their Wi-Fi (needed for push alerts). */
@Composable
fun WifiCard(ui: LockUiState, ble: LockBle) {
    var stage by remember { mutableStateOf(WifiStage.STATUS) }
    var selected by remember { mutableStateOf<WifiNet?>(null) }
    var password by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    // Last status that says something about the saved network (not a scan state).
    var lastConn by remember { mutableStateOf("") }

    val status = ui.wifiStatus
    LaunchedEffect(status) {
        if (status.isNotEmpty() && status != "scanning" && !status.startsWith("scan:done")) {
            lastConn = status
        }
        if (stage == WifiStage.CONNECTING) {
            when {
                status.startsWith("ok:") -> {
                    stage = WifiStage.STATUS
                    password = ""
                    error = null
                }
                status.startsWith("fail:") -> {
                    error = wifiText(status).first
                    stage = if (status == "fail:password" && selected?.locked == true)
                        WifiStage.PASSWORD else WifiStage.LIST
                }
            }
        }
    }

    val headerStatus = if (status.startsWith("scan:done")) lastConn else status
    val (text, color) = wifiText(headerStatus)
    val scanning = status == "scanning"

    fun connectTo(net: WifiNet, pass: String) {
        error = null
        if (ble.connectWifi(net.ssid, pass)) stage = WifiStage.CONNECTING
        else error = "Name or password is too long"
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(24.dp))
            .background(Card)
            .padding(18.dp)
    ) {
        Text("LOCK WI-FI", color = Muted, fontSize = 12.sp, letterSpacing = 3.sp)
        Spacer(Modifier.height(10.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(10.dp).clip(CircleShape).background(color))
            Spacer(Modifier.width(10.dp))
            Text(text, color = TextHi, fontSize = 14.sp)
        }
        error?.let {
            Spacer(Modifier.height(6.dp))
            Text(it, color = Bad, fontSize = 13.sp)
        }
        Spacer(Modifier.height(12.dp))

        when (stage) {
            WifiStage.STATUS -> {
                if (status != "fail:unsupported") {
                    val saved = lastConn.startsWith("ok:") || lastConn.startsWith("saved:")
                    Button(
                        onClick = { error = null; ble.scanWifi(); stage = WifiStage.LIST },
                        enabled = status.isNotEmpty(),
                        modifier = Modifier.fillMaxWidth().height(48.dp),
                        shape = RoundedCornerShape(14.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = Accent, contentColor = Bg)
                    ) {
                        Text(if (saved) "Change Wi-Fi" else "Set up Wi-Fi", fontWeight = FontWeight.SemiBold)
                    }
                }
            }

            WifiStage.LIST -> {
                when {
                    scanning || (!status.startsWith("scan:done") && ui.wifiNetworks.isEmpty() && error == null) ->
                        SmallSpinner("Searching for networks…")
                    ui.wifiNetworks.isEmpty() ->
                        Text(
                            "No networks found. Move the lock closer to the router and scan again.",
                            color = Muted,
                            fontSize = 13.sp
                        )
                    else -> {
                        Text("Choose your network", color = Muted, fontSize = 13.sp)
                        Spacer(Modifier.height(6.dp))
                        ui.wifiNetworks.forEach { net ->
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clip(RoundedCornerShape(12.dp))
                                    .clickable {
                                        selected = net
                                        error = null
                                        password = ""
                                        showPassword = false
                                        if (net.locked) stage = WifiStage.PASSWORD else connectTo(net, "")
                                    }
                                    .padding(horizontal = 8.dp, vertical = 12.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                SignalBars(net.rssi)
                                Spacer(Modifier.width(12.dp))
                                Text(
                                    net.ssid,
                                    color = TextHi,
                                    fontSize = 15.sp,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.weight(1f)
                                )
                                if (net.locked) Text("🔒", fontSize = 13.sp, color = Muted)
                            }
                        }
                    }
                }
                Spacer(Modifier.height(8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
                    OutlinedButton(
                        onClick = { error = null; stage = WifiStage.STATUS },
                        modifier = Modifier.weight(1f).height(46.dp),
                        shape = RoundedCornerShape(14.dp)
                    ) { Text("Cancel") }
                    OutlinedButton(
                        onClick = { error = null; ble.scanWifi() },
                        enabled = !scanning,
                        modifier = Modifier.weight(1f).height(46.dp),
                        shape = RoundedCornerShape(14.dp)
                    ) { Text("Scan again") }
                }
            }

            WifiStage.PASSWORD -> {
                val net = selected
                if (net == null) {
                    LaunchedEffect(Unit) { stage = WifiStage.LIST }
                } else {
                    Text("Password for ${net.ssid}", color = TextHi, fontSize = 14.sp)
                    Spacer(Modifier.height(8.dp))
                    OutlinedTextField(
                        value = password,
                        onValueChange = { if (it.toByteArray(Charsets.UTF_8).size <= 63) password = it },
                        singleLine = true,
                        placeholder = { Text("Wi-Fi password", color = Muted) },
                        visualTransformation =
                            if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                        trailingIcon = {
                            TextButton(onClick = { showPassword = !showPassword }) {
                                Text(if (showPassword) "Hide" else "Show", color = Accent, fontSize = 13.sp)
                            }
                        },
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedTextColor = TextHi,
                            unfocusedTextColor = TextHi,
                            focusedBorderColor = Accent,
                            unfocusedBorderColor = Muted,
                            cursorColor = Accent
                        ),
                        modifier = Modifier.fillMaxWidth()
                    )
                    Spacer(Modifier.height(12.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
                        OutlinedButton(
                            onClick = { error = null; stage = WifiStage.LIST },
                            modifier = Modifier.weight(1f).height(48.dp),
                            shape = RoundedCornerShape(14.dp)
                        ) { Text("Back") }
                        Button(
                            onClick = { connectTo(net, password) },
                            enabled = password.isNotEmpty(),
                            modifier = Modifier.weight(1f).height(48.dp),
                            shape = RoundedCornerShape(14.dp),
                            colors = ButtonDefaults.buttonColors(containerColor = Accent, contentColor = Bg)
                        ) { Text("Connect", fontWeight = FontWeight.SemiBold) }
                    }
                }
            }

            WifiStage.CONNECTING ->
                SmallSpinner("Testing ${selected?.ssid ?: "the network"}… this can take up to 20 s")
        }
    }
}
