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
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
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

data class LockUiState(
    val status: LockStatus = LockStatus.IDLE,
    val code: String = "----",
    val updatedAt: String? = null
)

/**
 * Talks to the ESP32 lock. Scans for the lock's service UUID, connects,
 * subscribes to the characteristic and exposes the latest code as a StateFlow.
 * Permissions are checked by the UI before start() is called.
 */
@SuppressLint("MissingPermission")
class LockBle(private val context: Context) {

    private val serviceUuid = UUID.fromString("4fafc201-1fb5-459e-8fcc-c5c9c331914b")
    private val charUuid    = UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a8")
    private val cccdUuid    = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private val main = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private var pairAttempts = 0
    private var gotValue = false         // true once a code arrived on this connection

    private val _state = MutableStateFlow(LockUiState())
    val state: StateFlow<LockUiState> = _state.asStateFlow()

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
        _state.update { it.copy(status = LockStatus.CONNECTING) }
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    private fun close() {
        stopScan()
        val g = gatt
        gatt = null                      // callbacks from this gatt are now ignored
        g?.disconnect()
        g?.close()
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (g !== gatt) return
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> g.discoverServices()
                BluetoothProfile.STATE_DISCONNECTED -> {
                    gatt = null
                    g.close()
                    // Bonded phone, dropped before any code arrived, and not a range
                    // timeout: the lock rejected our stored keys, i.e. it was wiped.
                    val stale = !gotValue && status != GATT_CONN_TIMEOUT &&
                        g.device.bondState == BluetoothDevice.BOND_BONDED
                    _state.update {
                        it.copy(status = if (stale) LockStatus.REPAIR_NEEDED else LockStatus.DISCONNECTED)
                    }
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (g !== gatt) return
            val ch = g.getService(serviceUuid)?.getCharacteristic(charUuid)
            if (status != BluetoothGatt.GATT_SUCCESS || ch == null) {
                _state.update { it.copy(status = LockStatus.NOT_FOUND) }
                return
            }
            enableNotifications(g, ch)
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int
        ) {
            if (g !== gatt) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                handleAuthFailure(g)          // lock wants us to pair first
                return
            }
            // Notifications are on; now read the current value once.
            g.getService(serviceUuid)?.getCharacteristic(charUuid)?.let { g.readCharacteristic(it) }
        }

        // Android 13+ delivers the value directly...
        override fun onCharacteristicRead(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray, status: Int
        ) {
            if (g !== gatt) return
            if (status == BluetoothGatt.GATT_SUCCESS) handleValue(value) else handleAuthFailure(g)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray
        ) {
            if (g === gatt) handleValue(value)
        }

        // ...older versions use these deprecated overloads.
        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicRead(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int
        ) {
            if (g !== gatt) return
            if (status == BluetoothGatt.GATT_SUCCESS) handleValue(ch.value ?: return)
            else handleAuthFailure(g)
        }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (g === gatt) handleValue(ch.value ?: return)
        }
    }

    @Suppress("DEPRECATION")
    private fun enableNotifications(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
        g.setCharacteristicNotification(ch, true)
        val d = ch.getDescriptor(cccdUuid)
        if (d == null) {
            g.readCharacteristic(ch)
            return
        }
        d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        g.writeDescriptor(d)
    }

    /**
     * The lock requires pairing (passkey). Android normally shows the passkey
     * dialog by itself; if it did not, we start bonding ourselves, then retry
     * the subscribe step every 2 s (up to ~30 s) until the pairing is done.
     */
    private fun handleAuthFailure(g: BluetoothGatt) {
        if (g !== gatt) return
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
            if (g === gatt) {
                g.getService(serviceUuid)?.getCharacteristic(charUuid)
                    ?.let { enableNotifications(g, it) }
            }
        }, 2000)
    }

    private fun handleValue(bytes: ByteArray) {
        pairAttempts = 0
        gotValue = true
        val text = String(bytes, Charsets.UTF_8).trim().ifEmpty { "----" }
        val time = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())
        _state.update { LockUiState(LockStatus.CONNECTED, text, time) }
    }

    private companion object {
        const val SCAN_TIMEOUT_MS = 20_000L
        const val MAX_PAIR_ATTEMPTS = 15
        const val GATT_CONN_TIMEOUT = 8      // link lost / out of range
    }
}
