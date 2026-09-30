package com.hev.uvcfieldmonitor

import android.Manifest
import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.TextView
import org.json.JSONObject
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption


class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {

        private const val TAG = "UvcUsb"

        private const val ACTION_USB_PERMISSION =
            "com.hev.uvcfieldmonitor.USB_PERMISSION"

        private const val REQUEST_CAMERA_PERMISSION = 1001
        private const val REQUEST_PROFILE_DOCUMENT = 1002

        init {
            System.loadLibrary("uvcfieldmonitor")
        }
    }


    private lateinit var surfaceView: SurfaceView
    private lateinit var profileStatusView: TextView

    private lateinit var usbManager: UsbManager

    private var usbConnection: UsbDeviceConnection? = null

    private var selectedUsbDevice: UsbDevice? = null
    private var loadedProfile: JSONObject? = null

    // Step 15.7.1: do not tear down/recreate EGL for duplicate
    // SurfaceHolder callbacks with the same buffer size.
    private var surfaceAttached = false
    private var lastSurfaceWidth = -1
    private var lastSurfaceHeight = -1


    external fun nativeSetSurface(surface: Surface?)
    external fun nativeOpenUsb(fd: Int): Boolean
    external fun nativeCloseUsb()
    external fun nativeConfigureCalibrationProfile(
        matrix: FloatArray,
        offsetCode: FloatArray,
        pu: IntArray,
        pqInput: Boolean,
        colorimetry: Int,
        width: Int,
        height: Int,
        limitedInput: Boolean
    ): Boolean
    external fun nativeDisableCalibrationProfile()


    // ------------------------------------------------------------
    // USB permission result receiver
    // ------------------------------------------------------------

    private val usbPermissionReceiver =
        object : BroadcastReceiver() {

            override fun onReceive(
                context: Context,
                intent: Intent
            ) {

                if (intent.action != ACTION_USB_PERMISSION) {
                    return
                }


                val device =
                    getUsbDeviceFromIntent(intent)


                val granted =
                    intent.getBooleanExtra(
                        UsbManager.EXTRA_PERMISSION_GRANTED,
                        false
                    )


                if (!granted) {

                    Log.e(
                        TAG,
                        "USB permission DENIED: $device"
                    )

                    return
                }


                if (device == null) {

                    Log.e(
                        TAG,
                        "USB permission granted but device is null"
                    )

                    return
                }


                Log.i(
                    TAG,
                    "USB permission GRANTED"
                )


                openUsbDevice(device)
            }
        }


    // ------------------------------------------------------------
    // Activity
    // ------------------------------------------------------------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)


        // Step 15.7.1: orientation policy lives in AndroidManifest.xml.
        // Establishing fullSensor before Activity/Surface creation avoids the
        // transient landscape Surface -> portrait Surface churn seen on A202ZT.

        window.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
        )


        surfaceView =
            SurfaceView(this)

        surfaceView.holder.addCallback(this)

        val root = FrameLayout(this)
        root.addView(
            surfaceView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
        )
        val importButton = Button(this).apply {
            text = "Import Profile"
            setOnClickListener { selectCalibrationProfile() }
        }
        root.addView(
            importButton,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START
            )
        )
        profileStatusView = TextView(this).apply {
            setTextColor(0xFFFFFFFF.toInt())
            setBackgroundColor(0x99000000.toInt())
            setPadding(16, 8, 16, 8)
            text = "Calibration: disabled"
        }
        root.addView(
            profileStatusView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM or Gravity.START
            )
        )
        setContentView(root)


        // PhoneWindow.getInsetsController() requires DecorView to exist.
        // Defer immersive setup until after setContentView() has created it.
        window.decorView.post {
            applyImmersiveFullscreen()
        }


        usbManager =
            getSystemService(
                Context.USB_SERVICE
            ) as UsbManager

        loadedProfile = loadSavedCalibrationProfile()
        if (loadedProfile != null) {
            profileStatusView.text = "Calibration profile loaded; waiting for device match"
        }


        registerUsbPermissionReceiver()


        Log.i(
            TAG,
            "USB Host feature = ${
                packageManager.hasSystemFeature(
                    PackageManager.FEATURE_USB_HOST
                )
            }"
        )


        ensureCameraPermissionThenScanUsb()
    }

    private fun selectCalibrationProfile() {
        startActivityForResult(
            Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "application/json"
            },
            REQUEST_PROFILE_DOCUMENT
        )
    }

    @Deprecated("Deprecated by Android; retained for minSdk-compatible document import")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_PROFILE_DOCUMENT || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try {
            val json = contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText() }
                ?: throw IllegalArgumentException("Profile document is empty")
            val profile = JSONObject(json)
            validateProfileStructure(profile)
            saveCalibrationProfile(json)
            loadedProfile = profile
            profileStatusView.text = "Profile imported; checking current device"
            val device = selectedUsbDevice
            if (device != null && usbManager.hasPermission(device)) {
                openUsbDevice(device)
            }
        } catch (error: Exception) {
            nativeDisableCalibrationProfile()
            profileStatusView.text = "PROFILE_INVALID: ${error.message}"
            Log.e(TAG, "Profile import failed", error)
        }
    }

    private fun profileFile() = filesDir.resolve("calibration_profile_v1.json")

    private fun saveCalibrationProfile(json: String) {
        val target = profileFile()
        val temporary = filesDir.resolve(target.name + ".tmp")
        FileOutputStream(temporary).use { output ->
            output.write(json.toByteArray(Charsets.UTF_8))
            output.fd.sync()
        }
        try {
            Files.move(
                temporary.toPath(), target.toPath(),
                StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING
            )
        } catch (_: AtomicMoveNotSupportedException) {
            Files.move(
                temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING
            )
        }
    }

    private fun loadSavedCalibrationProfile(): JSONObject? = try {
        val file = profileFile()
        if (!file.isFile) null else JSONObject(file.readText(Charsets.UTF_8)).also {
            validateProfileStructure(it)
        }
    } catch (error: Exception) {
        Log.e(TAG, "Saved calibration profile is invalid", error)
        null
    }

    private fun validateProfileStructure(profile: JSONObject) {
        require(profile.getInt("profileFormatVersion") == 1) { "Unsupported profile version" }
        val mode = profile.getJSONObject("captureMode")
        require(mode.getString("pixelFormat") == "MJPEG") { "Unsupported pixel format" }
        require(mode.getInt("width") == 1280 && mode.getInt("height") == 720) {
            "Unsupported resolution"
        }
        require(mode.getInt("frameRate") == 60) { "Unsupported frame rate" }
        val contract = profile.getJSONObject("inputContract")
        require(contract.getString("inputEncoding") in setOf("RGB", "YCBCR_444", "YCBCR_422")) {
            "Unsupported input encoding"
        }
        require(contract.getString("range") in setOf("FULL", "LIMITED")) {
            "Unsupported range"
        }
        require(contract.getString("colorimetry") in setOf("BT601", "BT709", "BT2020")) {
            "Unsupported colorimetry"
        }
        require(contract.getString("transferCharacteristics") in setOf("SDR", "PQ")) {
            "Unsupported transfer characteristics"
        }
        require(profile.getJSONObject("validation").getString("result") in
            setOf("CALIBRATION_VALID", "CALIBRATION_POOR_FIT")) {
            "Profile validation result is not applicable"
        }
        val correction = profile.getJSONObject("correction")
        require(correction.getJSONArray("matrix").length() == 3) { "Invalid matrix" }
        require(correction.getJSONArray("offsetCode").length() == 3) { "Invalid offset" }
    }

    private fun configureProfileForDevice(device: UsbDevice): Boolean {
        val profile = loadedProfile ?: run {
            nativeDisableCalibrationProfile()
            profileStatusView.text = "Calibration: disabled (no profile)"
            return false
        }
        return try {
            validateProfileStructure(profile)
            val identity = profile.getJSONObject("device")
            require(identity.getInt("vid") == device.vendorId &&
                identity.getInt("pid") == device.productId) { "VID/PID" }
            if (identity.has("serial")) {
                val actualSerial = usbConnection?.serial
                require(identity.getString("serial") == actualSerial) { "Serial" }
            }

            val correction = profile.getJSONObject("correction")
            val matrixJson = correction.getJSONArray("matrix")
            val matrix = FloatArray(9)
            for (row in 0 until 3) {
                val values = matrixJson.getJSONArray(row)
                require(values.length() == 3) { "Matrix shape" }
                for (column in 0 until 3) {
                    matrix[row * 3 + column] = values.getDouble(column).toFloat()
                }
            }
            val offsetJson = correction.getJSONArray("offsetCode")
            val offset = FloatArray(3) { offsetJson.getDouble(it).toFloat() }
            val puJson = profile.getJSONObject("pu")
            val pu = intArrayOf(
                puJson.getInt("brightness"), puJson.getInt("contrast"),
                puJson.getInt("saturation"), puJson.getInt("hue")
            )
            val pq = profile.getJSONObject("inputContract")
                .getString("transferCharacteristics") == "PQ"
            val inputContract = profile.getJSONObject("inputContract")
            val colorimetry = when (inputContract.getString("colorimetry")) {
                "BT601" -> 0
                "BT709" -> 1
                "BT2020" -> 2
                else -> error("Colorimetry")
            }
            val captureMode = profile.getJSONObject("captureMode")
            val limitedInput = inputContract.getString("range") == "LIMITED"
            require(nativeConfigureCalibrationProfile(
                matrix,
                offset,
                pu,
                pq,
                colorimetry,
                captureMode.getInt("width"),
                captureMode.getInt("height"),
                limitedInput
            )) {
                "Native profile configuration"
            }
            val quality = profile.getJSONObject("validation").getString("result")
            profileStatusView.text = "Calibration: enabled ($quality)"
            Log.i(TAG, "Phase 8 profile match: enabled ($quality)")
            true
        } catch (error: Exception) {
            nativeDisableCalibrationProfile()
            profileStatusView.text = "PROFILE_MODE_MISMATCH: ${error.message}; calibration disabled"
            Log.w(TAG, "PROFILE_MODE_MISMATCH: ${error.message}")
            false
        }
    }


    override fun onDestroy() {

        // libusb_wrap_sys_device() does not own the Android FD.
        // Close libusb first, then close UsbDeviceConnection.
        nativeCloseUsb()

        usbConnection?.close()
        usbConnection = null


        try {
            unregisterReceiver(
                usbPermissionReceiver
            )
        }
        catch (_: IllegalArgumentException) {
        }


        super.onDestroy()
    }


    // ------------------------------------------------------------
    // Orientation-aware / immersive fullscreen
    // ------------------------------------------------------------

    private fun applyImmersiveFullscreen() {

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {

            window.setDecorFitsSystemWindows(false)

            window.decorView.windowInsetsController?.let { controller ->

                controller.hide(
                    WindowInsets.Type.statusBars() or
                        WindowInsets.Type.navigationBars()
                )

                controller.systemBarsBehavior =
                    WindowInsetsController
                        .BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        }
        else {

            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility =
                View.SYSTEM_UI_FLAG_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        }


        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {

            window.attributes =
                window.attributes.apply {

                    layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams
                            .LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                }
        }
    }


    override fun onWindowFocusChanged(
        hasFocus: Boolean
    ) {
        super.onWindowFocusChanged(hasFocus)

        if (hasFocus) {
            applyImmersiveFullscreen()
        }
    }


    // ------------------------------------------------------------
    // Surface
    // ------------------------------------------------------------

    override fun surfaceCreated(
        holder: SurfaceHolder
    ) {

        val frame = holder.surfaceFrame
        lastSurfaceWidth = frame.width().takeIf { it > 0 } ?: -1
        lastSurfaceHeight = frame.height().takeIf { it > 0 } ?: -1
        surfaceAttached = true

        Log.i(
            TAG,
            "Surface created: ${lastSurfaceWidth}x${lastSurfaceHeight}"
        )

        nativeSetSurface(
            holder.surface
        )
    }


    override fun surfaceChanged(
        holder: SurfaceHolder,
        format: Int,
        width: Int,
        height: Int
    ) {

        Log.i(
            TAG,
            "Surface changed: ${width}x${height}"
        )

        if (!surfaceAttached) {
            surfaceAttached = true
            lastSurfaceWidth = width
            lastSurfaceHeight = height
            nativeSetSurface(holder.surface)
            return
        }

        if (
            width == lastSurfaceWidth &&
            height == lastSurfaceHeight
        ) {
            Log.i(
                TAG,
                "Step 15.7.1: duplicate Surface change ignored"
            )
            return
        }

        Log.i(
            TAG,
            "Step 15.7.1: Surface size changed " +
                "${lastSurfaceWidth}x${lastSurfaceHeight} -> ${width}x${height}; " +
                "restarting EGL once"
        )

        lastSurfaceWidth = width
        lastSurfaceHeight = height

        // A real orientation/size change still requires EGL/layout refresh.
        nativeSetSurface(
            holder.surface
        )
    }


    override fun surfaceDestroyed(
        holder: SurfaceHolder
    ) {

        surfaceAttached = false
        lastSurfaceWidth = -1
        lastSurfaceHeight = -1

        nativeSetSurface(null)
    }


    // ------------------------------------------------------------
    // CAMERA permission
    //
    // AndroidではUSB_CLASS_VIDEOへのpermission取得にも
    // CAMERA permissionが必要。
    // ------------------------------------------------------------

    private fun ensureCameraPermissionThenScanUsb() {

        if (
            checkSelfPermission(
                Manifest.permission.CAMERA
            ) == PackageManager.PERMISSION_GRANTED
        ) {

            Log.i(
                TAG,
                "CAMERA permission = GRANTED"
            )

            scanUsbDevices()

            return
        }


        Log.i(
            TAG,
            "Requesting CAMERA permission"
        )


        requestPermissions(
            arrayOf(
                Manifest.permission.CAMERA
            ),
            REQUEST_CAMERA_PERMISSION
        )
    }


    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {

        super.onRequestPermissionsResult(
            requestCode,
            permissions,
            grantResults
        )


        if (
            requestCode !=
            REQUEST_CAMERA_PERMISSION
        ) {
            return
        }


        if (
            grantResults.isNotEmpty() &&
            grantResults[0] ==
            PackageManager.PERMISSION_GRANTED
        ) {

            Log.i(
                TAG,
                "CAMERA permission GRANTED"
            )

            scanUsbDevices()
        }
        else {

            Log.e(
                TAG,
                "CAMERA permission DENIED"
            )
        }
    }


    // ------------------------------------------------------------
    // USB permission BroadcastReceiver registration
    // ------------------------------------------------------------

    private fun registerUsbPermissionReceiver() {

        val filter =
            IntentFilter(
                ACTION_USB_PERMISSION
            )


        if (
            Build.VERSION.SDK_INT >=
            Build.VERSION_CODES.TIRAMISU
        ) {

            registerReceiver(
                usbPermissionReceiver,
                filter,
                Context.RECEIVER_NOT_EXPORTED
            )
        }
        else {

            @Suppress("DEPRECATION")

            registerReceiver(
                usbPermissionReceiver,
                filter
            )
        }
    }


    // ------------------------------------------------------------
    // Enumerate USB devices
    // ------------------------------------------------------------

    private fun scanUsbDevices() {

        val devices =
            usbManager.deviceList.values


        Log.i(
            TAG,
            "USB device count = ${devices.size}"
        )


        if (devices.isEmpty()) {

            Log.e(
                TAG,
                "No USB devices found"
            )

            return
        }


        var uvcCandidate: UsbDevice? = null


        for (device in devices) {

            logUsbDevice(device)


            if (
                uvcCandidate == null &&
                hasVideoInterface(device)
            ) {

                uvcCandidate = device
            }
        }


        if (uvcCandidate == null) {

            Log.e(
                TAG,
                "No USB video-class device found"
            )

            return
        }


        selectedUsbDevice =
            uvcCandidate


        Log.i(
            TAG,
            "Selected UVC device:"
        )

        Log.i(
            TAG,
            "VID=0x%04X PID=0x%04X name=%s".format(
                uvcCandidate.vendorId,
                uvcCandidate.productId,
                uvcCandidate.deviceName
            )
        )


        requestUsbPermission(
            uvcCandidate
        )
    }


    // ------------------------------------------------------------
    // Does device contain UVC interface?
    // ------------------------------------------------------------

    private fun hasVideoInterface(
        device: UsbDevice
    ): Boolean {

        for (
        index in
        0 until device.interfaceCount
        ) {

            val intf =
                device.getInterface(index)


            if (
                intf.interfaceClass ==
                UsbConstants.USB_CLASS_VIDEO
            ) {

                return true
            }
        }


        return false
    }


    // ------------------------------------------------------------
    // Descriptor logging
    // ------------------------------------------------------------

    private fun logUsbDevice(
        device: UsbDevice
    ) {

        Log.i(
            TAG,
            "--------------------------------------------------"
        )


        Log.i(
            TAG,
            "USB Device:"
        )


        Log.i(
            TAG,
            "name=${device.deviceName}"
        )


        Log.i(
            TAG,
            "VID=0x%04X PID=0x%04X".format(
                device.vendorId,
                device.productId
            )
        )


        Log.i(
            TAG,
            "deviceClass=${device.deviceClass} " +
                    "subClass=${device.deviceSubclass} " +
                    "protocol=${device.deviceProtocol}"
        )


        Log.i(
            TAG,
            "interfaces=${device.interfaceCount}"
        )


        for (
        i in
        0 until device.interfaceCount
        ) {

            val intf =
                device.getInterface(i)


            Log.i(
                TAG,
                "Interface[$i]: " +
                        "id=${intf.id} " +
                        "alt=${intf.alternateSetting} " +
                        "class=${intf.interfaceClass} " +
                        "subClass=${intf.interfaceSubclass} " +
                        "protocol=${intf.interfaceProtocol} " +
                        "endpoints=${intf.endpointCount}"
            )


            for (
            e in
            0 until intf.endpointCount
            ) {

                val ep =
                    intf.getEndpoint(e)


                Log.i(
                    TAG,
                    "  Endpoint[$e]: " +
                            "address=0x%02X ".format(
                                ep.address
                            ) +
                            "dir=${usbDirectionName(ep.direction)} " +
                            "type=${usbTransferTypeName(ep.type)} " +
                            "maxPacket=${ep.maxPacketSize} " +
                            "interval=${ep.interval}"
                )
            }
        }
    }


    // ------------------------------------------------------------
    // USB permission
    // ------------------------------------------------------------

    private fun requestUsbPermission(
        device: UsbDevice
    ) {

        if (
            usbManager.hasPermission(device)
        ) {

            Log.i(
                TAG,
                "USB permission already granted"
            )

            openUsbDevice(device)

            return
        }


        val permissionIntent =
            PendingIntent.getBroadcast(
                this,
                0,
                Intent(
                    ACTION_USB_PERMISSION
                ).setPackage(
                    packageName
                ),
                PendingIntent.FLAG_MUTABLE
            )

        Log.i(
            TAG,
            "Requesting USB permission"
        )


        usbManager.requestPermission(
            device,
            permissionIntent
        )
    }


    // ------------------------------------------------------------
    // Open USB device
    // ------------------------------------------------------------

    private fun openUsbDevice(
        device: UsbDevice
    ) {

        // If reopening, release the libusb wrapper before closing
        // the Android-owned file descriptor.
        nativeCloseUsb()

        usbConnection?.close()
        usbConnection = null


        val connection =
            usbManager.openDevice(device)


        if (connection == null) {

            Log.e(
                TAG,
                "usbManager.openDevice() failed"
            )

            return
        }


        // Android 16 / vendor-kernel ownership test:
        // force=true asks the framework to disconnect a bound kernel
        // driver if the interface is busy, then claim it for this
        // UsbDeviceConnection.  Keep the claims alive while libusb wraps
        // the same Android-owned file descriptor.
        if (!claimUvcInterfacesForNative(connection, device)) {

            Log.e(
                TAG,
                "Android force-claim test: VS IF1 claim failed; aborting USB open"
            )

            connection.close()
            return
        }


        usbConnection =
            connection

        configureProfileForDevice(device)


        val fd =
            connection.fileDescriptor


        Log.i(
            TAG,
            "USB DEVICE OPENED"
        )


        Log.i(
            TAG,
            "native FD = $fd"
        )


        val nativeResult =
            nativeOpenUsb(fd)

        Log.i(
            TAG,
            "nativeOpenUsb = $nativeResult"
        )


        Log.i(
            TAG,
            "raw descriptor bytes = ${
                connection.rawDescriptors?.size ?: 0
            }"
        )


        try {

            Log.i(
                TAG,
                "manufacturer=${device.manufacturerName}"
            )

            Log.i(
                TAG,
                "product=${device.productName}"
            )

            Log.i(
                TAG,
                "serial=${connection.serial}"
            )
        }
        catch (e: Exception) {

            Log.e(
                TAG,
                "String descriptor read failed: ${e.message}"
            )
        }
    }


    // ------------------------------------------------------------
    // Android 16 / vendor-kernel interface ownership test
    //
    // MS2109:
    //   IF0 ALT0 = VideoControl
    //   IF1 ALT0 = VideoStreaming idle alternate setting
    //
    // IF0 is best-effort because PU controls are non-fatal.
    // IF1 is required because native libusb streaming needs it.
    // ------------------------------------------------------------

    private fun claimUvcInterfacesForNative(
        connection: UsbDeviceConnection,
        device: UsbDevice
    ): Boolean {

        var vcInterface: UsbInterface? = null
        var vsInterface: UsbInterface? = null


        for (i in 0 until device.interfaceCount) {

            val intf =
                device.getInterface(i)

            if (
                intf.id == 0 &&
                intf.alternateSetting == 0
            ) {
                vcInterface = intf
            }

            if (
                intf.id == 1 &&
                intf.alternateSetting == 0
            ) {
                vsInterface = intf
            }
        }


        if (vcInterface == null) {

            Log.e(
                TAG,
                "Android force-claim test: VC IF0 ALT0 not found"
            )
        }
        else {

            val vcClaimed =
                connection.claimInterface(
                    vcInterface,
                    true
                )

            Log.i(
                TAG,
                "Android force-claim VC IF0 ALT0 force=true -> $vcClaimed"
            )

            if (!vcClaimed) {
                Log.e(
                    TAG,
                    "Android force-claim test: VC IF0 failed; PU controls may remain unavailable"
                )
            }
        }


        if (vsInterface == null) {

            Log.e(
                TAG,
                "Android force-claim test: VS IF1 ALT0 not found"
            )

            return false
        }


        val vsClaimed =
            connection.claimInterface(
                vsInterface,
                true
            )

        Log.i(
            TAG,
            "Android force-claim VS IF1 ALT0 force=true -> $vsClaimed"
        )


        return vsClaimed
    }


    // ------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------

    private fun usbDirectionName(
        direction: Int
    ): String {

        return when (direction) {

            UsbConstants.USB_DIR_IN ->
                "IN"

            UsbConstants.USB_DIR_OUT ->
                "OUT"

            else ->
                "UNKNOWN($direction)"
        }
    }


    private fun usbTransferTypeName(
        type: Int
    ): String {

        return when (type) {

            UsbConstants.USB_ENDPOINT_XFER_CONTROL ->
                "CONTROL"

            UsbConstants.USB_ENDPOINT_XFER_ISOC ->
                "ISO"

            UsbConstants.USB_ENDPOINT_XFER_BULK ->
                "BULK"

            UsbConstants.USB_ENDPOINT_XFER_INT ->
                "INT"

            else ->
                "UNKNOWN($type)"
        }
    }


    private fun getUsbDeviceFromIntent(
        intent: Intent
    ): UsbDevice? {

        return if (
            Build.VERSION.SDK_INT >=
            Build.VERSION_CODES.TIRAMISU
        ) {

            intent.getParcelableExtra(
                UsbManager.EXTRA_DEVICE,
                UsbDevice::class.java
            )
        }
        else {

            @Suppress("DEPRECATION")

            intent.getParcelableExtra(
                UsbManager.EXTRA_DEVICE
            )
        }
    }
}
