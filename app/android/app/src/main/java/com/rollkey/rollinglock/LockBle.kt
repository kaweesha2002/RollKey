package com.rollkey.rollinglock

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

enum class LockStatus {
    IDLE, BT_OFF, NO_PERMISSION, SCANNING, CONNECTING, PAIRING, PAIRING_FAILED,
    CONNECTED, DISCONNECTED, NOT_FOUND,
    /** Phone still holds an old pairing the lock no longer knows (lock was wiped). */
    REPAIR_NEEDED
}

/** One Wi-Fi network the lock found in a scan. */
data class WifiNet(val rssi: Int, val locked: Boolean, val ssid: String)

data class LockUiState(
    val status: LockStatus = LockStatus.IDLE,
    val code: String = "----",
    val updatedAt: String? = null,
    /** The lock's FCM alert topic, read over BLE. */
    val topic: String? = null,
    /** First line of WIFI_STATUS: none, saved:<ssid>, scanning, testing, ok:<ssid>, fail:<why>, scan:done. Empty = unknown. */
    val wifiStatus: String = "",
    /** Result of the last Wi-Fi scan, strongest first. */
    val wifiNetworks: List<WifiNet> = emptyList()
)

/**
 * Talks to the ESP32 lock. Scans for the lock's service UUID, connects,
 * subscribes to the code characteristic and exposes the latest code as a StateFlow.
 * Also reads the lock's alert topic and drives Wi-Fi setup.
 * Permissions are checked by the UI before start() is called.
 *
 * Android runs only one GATT operation at a time, so every MTU request,
 * descriptor write, read and write goes through a small serial queue that
 * advances when the matching callback arrives.
 */
@SuppressLint("MissingPermission")
class LockBle(private val context: Context) {

    private val serviceUuid    = UUID.fromString("4fafc201-1fb5-459e-8fcc-c5c9c331914b")
    private val charUuid       = UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a8")
    private val topicUuid      = UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a9")
    private val wifiCtrlUuid   = UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26aa")
    private val wifiStatusUuid = UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26ab")
    private val cccdUuid       = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private val main = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private var pairAttempts = 0
    private var gotValue = false         // true once a code arrived on this connection
    private var cacheRefreshed = false   // GATT cache already refreshed on this connection

    private val _state = MutableStateFlow(LockUiState())
    val state: StateFlow<LockUiState> = _state.asStateFlow()

    // ---------------- GATT operation queue ----------------

    private sealed class Op {
        object Mtu : Op()
        data class Notify(val uuid: UUID) : Op()
        data class Read(val uuid: UUID) : Op()
        class Write(val uuid: UUID, val value: ByteArray) : Op()
    }

    private val queue = ArrayDeque<Op>()
    private var current: Op? = null

    // ---------------- Public API ----------------

    fun start() {
        if (adapter == null || !adapter.isEnabled) {
            _state.update { it.copy(status = LockStatus.BT_OFF) }
            return
        }
        close()
        scanning = true
        _state.update { it.copy(status = LockStatus.SCANNING) }

        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(serviceUuid)).build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        adapter.bluetoothLeScanner?.startScan(listOf(filter), settings, scanCallback)

        main.postDelayed(scanTimeout, SCAN_TIMEOUT_MS)
    }

    /** User pressed Cancel / Disconnect. Keeps the last code on screen. */
    fun stop() {
        close()
        _state.update { it.copy(status = LockStatus.IDLE) }
    }

    fun permissionDenied() {
        _state.update { it.copy(status = LockStatus.NO_PERMISSION) }
    }

    fun release() = close()

    /** Used when the app locks: drop the connection and forget the code on screen. */
    fun reset() {
        close()
        _state.value = LockUiState()
    }

    /** Asks the lock to scan for Wi-Fi networks. Result arrives as wifiStatus "scan:done" + wifiNetworks. */
    fun scanWifi() {
        main.post {
            if (_state.value.status != LockStatus.CONNECTED || gatt == null) return@post
            _state.update { it.copy(wifiStatus = "scanning", wifiNetworks = emptyList()) }
            enqueue(Op.Write(wifiCtrlUuid, byteArrayOf('S'.code.toByte())))
        }
    }

    /**
     * Sends a network to the lock. The lock tests it and only saves it if it works.
     * Returns false if the SSID / password are too long to send.
     */
    fun connectWifi(ssid: String, password: String): Boolean {
        val s = ssid.toByteArray(Charsets.UTF_8)
        val p = password.toByteArray(Charsets.UTF_8)
        if (s.isEmpty() || s.size > MAX_SSID_BYTES || p.size > MAX_PASS_BYTES) return false
        val payload = byteArrayOf('C'.code.toByte(), s.size.toByte()) + s + p
        main.post {
            if (_state.value.status != LockStatus.CONNECTED || gatt == null) return@post
            _state.update { it.copy(wifiStatus = "testing") }
            enqueue(Op.Write(wifiCtrlUuid, payload))
        }
        return true
    }

    // ---------------- Scanning ----------------

    private val scanTimeout = Runnable {
        if (scanning) {
            stopScan()
            _state.update { it.copy(status = LockStatus.NOT_FOUND) }
        }
    }

    private fun stopScan() {
        main.removeCallbacks(scanTimeout)
        if (scanning) {
            scanning = false
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!scanning) return
            stopScan()
            connect(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            main.removeCallbacks(scanTimeout)
            _state.update { it.copy(status = LockStatus.NOT_FOUND) }
        }
    }

    // ---------------- Connection ----------------

    private fun connect(device: BluetoothDevice) {
        pairAttempts = 0
        gotValue = false
        cacheRefreshed = false
        clearQueue()
        _state.update {
            it.copy(status = LockStatus.CONNECTING, wifiStatus = "", wifiNetworks = emptyList())
        }
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    private fun close() {
        stopScan()
        clearQueue()
        val g = gatt
        gatt = null                      // callbacks from this gatt are now ignored
        g?.disconnect()
        g?.close()
    }

    /** All GATT callbacks are moved to the main thread, so the queue needs no locking. */
    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            main.post { onConnectionState(g, status, newState) }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post { onServices(g, status) }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            main.post { if (g === gatt && current is Op.Mtu) next() }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int
        ) {
            main.post { onOpResult(g, status) }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int
        ) {
            main.post {
                if (g !== gatt) return@post
                if (status != BluetoothGatt.GATT_SUCCESS && ch.uuid == wifiCtrlUuid) {
                    _state.update { it.copy(wifiStatus = "fail:send") }
                }
                onOpResult(g, status)
            }
        }

        // Android 13+ delivers the value directly...
        override fun onCharacteristicRead(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray, status: Int
        ) {
            val copy = value.copyOf()
            main.post { onRead(g, ch.uuid, copy, status) }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray
        ) {
            val copy = value.copyOf()
            main.post { onChanged(g, ch.uuid, copy) }
        }

        // ...older versions use these deprecated overloads.
        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicRead(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int
        ) {
            val copy = ch.value?.copyOf() ?: ByteArray(0)
            main.post { onRead(g, ch.uuid, copy, status) }
        }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            val copy = ch.value?.copyOf() ?: return
            main.post { onChanged(g, ch.uuid, copy) }
        }
    }

    private fun onConnectionState(g: BluetoothGatt, status: Int, newState: Int) {
        if (g !== gatt) return
        when (newState) {
            BluetoothProfile.STATE_CONNECTED -> g.discoverServices()
            BluetoothProfile.STATE_DISCONNECTED -> {
                gatt = null
                clearQueue()
                g.close()
                // Bonded phone, dropped before any code arrived, and not a range
                // timeout: the lock rejected our stored keys, i.e. it was wiped.
                val stale = !gotValue && status != GATT_CONN_TIMEOUT &&
                    g.device.bondState == BluetoothDevice.BOND_BONDED
                _state.update {
                    it.copy(
                        status = if (stale) LockStatus.REPAIR_NEEDED else LockStatus.DISCONNECTED,
                        wifiStatus = "",
                        wifiNetworks = emptyList()
                    )
                }
            }
        }
    }

    private fun onServices(g: BluetoothGatt, status: Int) {
        if (g !== gatt) return
        val svc = g.getService(serviceUuid)
        val ch = svc?.getCharacteristic(charUuid)
        if (status != BluetoothGatt.GATT_SUCCESS || ch == null) {
            _state.update { it.copy(status = LockStatus.NOT_FOUND) }
            return
        }
        // Android caches a paired device's services. After a firmware update the
        // cache can still hold the old list (no topic / Wi-Fi characteristics).
        // Refresh it once and discover again instead of asking the user to unpair.
        val missing = svc.getCharacteristic(topicUuid) == null ||
            svc.getCharacteristic(wifiStatusUuid) == null
        if (missing && !cacheRefreshed) {
            cacheRefreshed = true
            if (refreshGattCache(g)) {
                main.postDelayed({ if (g === gatt) g.discoverServices() }, 600)
                return
            }
        }
        queue.add(Op.Mtu)
        queueSetup()
    }

    /**
     * Clears Android's cached service list for this device. BluetoothGatt.refresh()
     * is a hidden but long-standing API (used by Nordic's BLE library); returns
     * false if this Android version blocks it.
     */
    private fun refreshGattCache(g: BluetoothGatt): Boolean = try {
        g.javaClass.getMethod("refresh").invoke(g) as? Boolean ?: false
    } catch (_: Exception) {
        false
    }

    /** Subscribe + read everything. Also used to retry after the pairing finished. */
    private fun queueSetup() {
        queue.add(Op.Notify(charUuid))
        queue.add(Op.Read(charUuid))
        queue.add(Op.Read(topicUuid))
        queue.add(Op.Notify(wifiStatusUuid))
        queue.add(Op.Read(wifiStatusUuid))
        if (current == null) next()
    }

    private fun onOpResult(g: BluetoothGatt, status: Int) {
        if (g !== gatt) return
        if (status != BluetoothGatt.GATT_SUCCESS && !gotValue) {
            handleAuthFailure(g)          // lock wants us to pair first
            return
        }
        next()
    }

    private fun onRead(g: BluetoothGatt, uuid: UUID, value: ByteArray, status: Int) {
        if (g !== gatt) return
        if (status != BluetoothGatt.GATT_SUCCESS) {
            onOpResult(g, status)
            return
        }
        handleCharValue(uuid, value, fromRead = true)
        next()
    }

    private fun onChanged(g: BluetoothGatt, uuid: UUID, value: ByteArray) {
        if (g === gatt) handleCharValue(uuid, value, fromRead = false)
    }

    private fun enqueue(op: Op) {
        queue.add(op)
        if (current == null) next()
    }

    private fun clearQueue() {
        queue.clear()
        current = null
    }

    /** Starts the next queued operation. Operations that can't start are skipped. */
    private fun next() {
        current = null
        val g = gatt ?: run { queue.clear(); return }
        while (queue.isNotEmpty()) {
            val op = queue.removeFirst()
            current = op
            if (startOp(g, op)) return
            current = null
        }
    }

    @Suppress("DEPRECATION")
    private fun startOp(g: BluetoothGatt, op: Op): Boolean {
        val service = g.getService(serviceUuid) ?: return false
        return when (op) {
            is Op.Mtu -> g.requestMtu(MTU)
            is Op.Read -> {
                val ch = service.getCharacteristic(op.uuid) ?: return false   // older firmware
                g.readCharacteristic(ch)
            }
            is Op.Notify -> {
                val ch = service.getCharacteristic(op.uuid) ?: return false
                g.setCharacteristicNotification(ch, true)
                val d = ch.getDescriptor(cccdUuid) ?: return false
                val v = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                if (Build.VERSION.SDK_INT >= 33) {
                    g.writeDescriptor(d, v) == BluetoothStatusCodes.SUCCESS
                } else {
                    d.value = v
                    g.writeDescriptor(d)
                }
            }
            is Op.Write -> {
                val ch = service.getCharacteristic(op.uuid)
                if (ch == null) {
                    if (op.uuid == wifiCtrlUuid) _state.update { it.copy(wifiStatus = "fail:unsupported") }
                    return false
                }
                val type = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                val ok = if (Build.VERSION.SDK_INT >= 33) {
                    g.writeCharacteristic(ch, op.value, type) == BluetoothStatusCodes.SUCCESS
                } else {
                    ch.writeType = type
                    ch.value = op.value
                    g.writeCharacteristic(ch)
                }
                if (!ok && op.uuid == wifiCtrlUuid) _state.update { it.copy(wifiStatus = "fail:send") }
                ok
            }
        }
    }

    /**
     * The lock requires pairing (passkey). Android normally shows the passkey
     * dialog by itself; if it did not, we start bonding ourselves, then retry
     * the subscribe step every 2 s (up to ~30 s) until the pairing is done.
     */
    private fun handleAuthFailure(g: BluetoothGatt) {
        if (g !== gatt) return
        clearQueue()
        if (pairAttempts++ >= MAX_PAIR_ATTEMPTS) {
            val stale = g.device.bondState == BluetoothDevice.BOND_BONDED
            _state.update {
                it.copy(status = if (stale) LockStatus.REPAIR_NEEDED else LockStatus.PAIRING_FAILED)
            }
            close()
            return
        }
        _state.update { it.copy(status = LockStatus.PAIRING) }
        if (g.device.bondState == BluetoothDevice.BOND_NONE) g.device.createBond()
        main.postDelayed({
            if (g === gatt && !gotValue) queueSetup()
        }, 2000)
    }

    private fun handleCharValue(uuid: UUID, bytes: ByteArray, fromRead: Boolean) {
        when (uuid) {
            charUuid -> handleCode(bytes)
            topicUuid -> {
                val topic = String(bytes, Charsets.UTF_8).trim()
                if (topic.isNotEmpty()) {
                    _state.update { it.copy(topic = topic) }
                    Alerts.onTopicFromLock(context, topic)
                }
            }
            wifiStatusUuid -> handleWifiStatus(bytes, fromRead)
        }
    }

    private fun handleCode(bytes: ByteArray) {
        pairAttempts = 0
        gotValue = true
        val text = String(bytes, Charsets.UTF_8).trim().ifEmpty { "----" }
        val time = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())
        _state.update { it.copy(status = LockStatus.CONNECTED, code = text, updatedAt = time) }
    }

    private fun handleWifiStatus(bytes: ByteArray, fromRead: Boolean) {
        val lines = String(bytes, Charsets.UTF_8).split('\n').map { it.trimEnd('\r') }
        val first = lines.firstOrNull()?.trim().orEmpty()
        if (first.startsWith("scan:done")) {
            if (!fromRead) {
                // Notifications are cut to the MTU: read the full list.
                enqueue(Op.Read(wifiStatusUuid))
                return
            }
            val nets = lines.drop(1).mapNotNull { parseNet(it) }
            _state.update { it.copy(wifiStatus = first, wifiNetworks = nets) }
        } else {
            _state.update {
                it.copy(
                    wifiStatus = first,
                    wifiNetworks = if (first == "scanning") emptyList() else it.wifiNetworks
                )
            }
        }
    }

    /** "<rssi>,<locked 0|1>,<ssid>" — the SSID may itself contain commas. */
    private fun parseNet(line: String): WifiNet? {
        val parts = line.split(',', limit = 3)
        if (parts.size < 3 || parts[2].isEmpty()) return null
        val rssi = parts[0].trim().toIntOrNull() ?: return null
        return WifiNet(rssi, parts[1].trim() == "1", parts[2])
    }

    private companion object {
        const val SCAN_TIMEOUT_MS = 20_000L
        const val MAX_PAIR_ATTEMPTS = 15
        const val GATT_CONN_TIMEOUT = 8      // link lost / out of range
        const val MTU = 247
        const val MAX_SSID_BYTES = 32
        const val MAX_PASS_BYTES = 63
    }
}
