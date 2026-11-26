package org.regml.app

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.hardware.input.InputManager
import android.os.Bundle
import android.provider.DocumentsContract
import android.text.InputType
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.view.inputmethod.InputConnection
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.io.FileOutputStream

class MainActivity : Activity() {
    private var gameSurface: GameSurface? = null
    private val chooseFolder = 41

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        showPicker()
    }

    private fun showPicker(message: String = "Pick the folder containing data.win and the game's audio files") {
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(32, 32, 32, 32)
            setBackgroundColor(Color.BLACK)
        }
        layout.addView(TextView(this).apply {
            text = message
            textSize = 18f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
        })
        layout.addView(Button(this).apply {
            text = "Choose game folder"
            setOnClickListener {
                val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
                startActivityForResult(intent, chooseFolder)
            }
        })
        setContentView(layout)
    }

    @Deprecated("The folder picker result is handled through the activity result callback")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != chooseFolder || resultCode != RESULT_OK || data?.data == null) return
        val tree = data.data!!
        try {
            contentResolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            val gameDirectory = File(filesDir, "game")
            gameDirectory.deleteRecursively()
            gameDirectory.mkdirs()
            val rootDocument = DocumentsContract.buildDocumentUriUsingTree(
                tree, DocumentsContract.getTreeDocumentId(tree)
            )
            copyDirectory(rootDocument, gameDirectory, 0)
            val packageFile = gameDirectory.walkTopDown().firstOrNull {
                it.isFile && it.name.equals("data.win", ignoreCase = true)
            } ?: error("No data.win found in that folder")
            gameSurface = GameSurface(this, packageFile, File(filesDir, "saves")) { error ->
                runOnUiThread {
                    gameSurface?.stopGame()
                    gameSurface = null
                    showPicker(error)
                }
            }
            setContentView(gameSurface)
        } catch (error: Exception) {
            showPicker(error.message ?: "Couldn't import that folder")
        }
    }

    private fun copyDirectory(directoryUri: android.net.Uri, destination: File, depth: Int) {
        require(depth < 32) { "Game folder nesting is too deep" }
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(
            directoryUri,
            DocumentsContract.getDocumentId(directoryUri)
        )
        contentResolver.query(
            children,
            arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE),
            null, null, null
        )?.use { cursor ->
            while (cursor.moveToNext()) {
                val id = cursor.getString(0)
                val name = cursor.getString(1)?.takeIf { it.isNotBlank() } ?: continue
                val safeName = name.replace('/', '_').replace('\\', '_')
                val child = DocumentsContract.buildDocumentUriUsingTree(directoryUri, id)
                val output = File(destination, safeName)
                if (cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) {
                    output.mkdirs()
                    copyDirectory(child, output, depth + 1)
                } else {
                    contentResolver.openInputStream(child)?.use { input ->
                        FileOutputStream(output).use { stream ->
                            val buffer = ByteArray(64 * 1024)
                            while (true) {
                                val read = input.read(buffer)
                                if (read < 0) break
                                stream.write(buffer, 0, read)
                            }
                        }
                    }
                }
            }
        } ?: error("Couldn't read the selected folder")
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (gameSurface?.handleKeyEvent(event) == true) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (gameSurface?.handleGenericMotionEvent(event) == true) return true
        return super.dispatchGenericMotionEvent(event)
    }

    override fun onBackPressed() {
        gameSurface?.stopGame()
        gameSurface = null
        showPicker()
    }
}

private class GameSurface(
    activity: Activity,
    private val packageFile: File,
    private val saveDirectory: File,
    private val onFailure: (String) -> Unit
) : View(activity) {
    private external fun nativeCreate(packagePath: String, savePath: String): Boolean
    private external fun nativeFrame(inputMask: Int, frame: IntArray)
    private external fun nativeSpeed(): Float
    private external fun nativeWidth(): Int
    private external fun nativeHeight(): Int
    private external fun nativeTypedKey(key: Int, character: Int)
    private external fun nativeSetKey(key: Int, down: Boolean)
    private external fun nativeSetGamepadState(
        connected: Boolean,
        buttonMask: Int,
        axisLX: Float,
        axisLY: Float,
        axisRX: Float,
        axisRY: Float
    )
    private external fun nativeDestroy()

    @Volatile private var running = true
    @Volatile private var touchMask = 0
    @Volatile private var physicalMask = 0
    @Volatile private var gamepadKeyMask = 0
    @Volatile private var gamepadConnected = false
    private var gamepadKeyButtons = 0
    private var gamepadAxisButtons = 0
    private var axisLX = 0f
    private var axisLY = 0f
    private var axisRX = 0f
    private var axisRY = 0f
    private val inputManager = activity.getSystemService(Context.INPUT_SERVICE) as InputManager
    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = refreshGamepadConnected()
        override fun onInputDeviceRemoved(deviceId: Int) = refreshGamepadConnected()
        override fun onInputDeviceChanged(deviceId: Int) = refreshGamepadConnected()
    }
    private var gameBitmap: android.graphics.Bitmap? = null
    private val paint = android.graphics.Paint().apply { isFilterBitmap = false }
    private val renderThread = Thread({ renderLoop() }, "reGML Android game")

    init {
        setBackgroundColor(Color.BLACK)
        isFocusable = true
        isFocusableInTouchMode = true
        renderThread.start()
    }

    fun setKey(bit: Int, down: Boolean) {
        physicalMask = if (down) physicalMask or bit else physicalMask and bit.inv()
    }

    fun typeKey(key: Int, character: Int = key) {
        nativeTypedKey(key, character)
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        inputManager.registerInputDeviceListener(inputDeviceListener, null)
        refreshGamepadConnected()
    }

    override fun onDetachedFromWindow() {
        inputManager.unregisterInputDeviceListener(inputDeviceListener)
        super.onDetachedFromWindow()
    }

    fun handleKeyEvent(event: KeyEvent): Boolean {
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) return false
        val down = event.action == KeyEvent.ACTION_DOWN
        val isGamepadSource = (event.source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (event.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        if (isGamepadSource && !gamepadConnected) {
            gamepadConnected = true
        }
        val gpBit = gamepadButtonBit(event.keyCode)
        if (gpBit != 0) {
            gamepadKeyButtons = if (down) gamepadKeyButtons or gpBit else gamepadKeyButtons and gpBit.inv()
            syncGamepadState()
        }
        val bit = when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> 1
            KeyEvent.KEYCODE_DPAD_UP -> 2
            KeyEvent.KEYCODE_DPAD_RIGHT -> 4
            KeyEvent.KEYCODE_DPAD_DOWN -> 8
            KeyEvent.KEYCODE_Z, KeyEvent.KEYCODE_BUTTON_A -> 16
            KeyEvent.KEYCODE_X, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_BACK -> 32
            KeyEvent.KEYCODE_C, KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y -> 64
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER, KeyEvent.KEYCODE_BUTTON_START -> 16 or 128
            else -> 0
        }
        if (bit != 0) {
            setKey(bit, down)
        }
        val gmlKey = gmlKeyCode(event.keyCode)
        if (gmlKey != 0) {
            nativeSetKey(gmlKey, down)
        }
        if (down) {
            if (event.unicodeChar in 32..126) {
                val key = if (gmlKey != 0) gmlKey else event.unicodeChar.toChar().uppercaseChar().code
                typeKey(key, event.unicodeChar)
                return true
            }
            if (event.keyCode == KeyEvent.KEYCODE_ENTER || event.keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER) {
                typeKey(13, 13)
                return true
            }
            if (event.keyCode == KeyEvent.KEYCODE_DEL) {
                typeKey(8, 8)
                return true
            }
        }
        return bit != 0 || gpBit != 0 || gmlKey != 0
    }

    fun handleGenericMotionEvent(event: MotionEvent): Boolean {
        if ((event.source and InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK ||
            event.action != MotionEvent.ACTION_MOVE) {
            return false
        }
        gamepadConnected = true
        axisLX = event.getAxisValue(MotionEvent.AXIS_X).coerceIn(-1f, 1f)
        axisLY = event.getAxisValue(MotionEvent.AXIS_Y).coerceIn(-1f, 1f)
        axisRX = event.getAxisValue(MotionEvent.AXIS_Z).coerceIn(-1f, 1f)
        axisRY = event.getAxisValue(MotionEvent.AXIS_RZ).coerceIn(-1f, 1f)
        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        val lTrigger = maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
        val rTrigger = maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
        var axisButtons = 0
        if (lTrigger > 0.25f) axisButtons = axisButtons or (1 shl 6)
        if (rTrigger > 0.25f) axisButtons = axisButtons or (1 shl 7)
        if (hatY < -0.5f) axisButtons = axisButtons or (1 shl 12)
        if (hatY > 0.5f) axisButtons = axisButtons or (1 shl 13)
        if (hatX < -0.5f) axisButtons = axisButtons or (1 shl 14)
        if (hatX > 0.5f) axisButtons = axisButtons or (1 shl 15)
        gamepadAxisButtons = axisButtons
        var keyMask = 0
        if (axisLX < -0.25f || hatX < -0.5f) keyMask = keyMask or 1
        if (axisLY < -0.25f || hatY < -0.5f) keyMask = keyMask or 2
        if (axisLX > 0.25f || hatX > 0.5f) keyMask = keyMask or 4
        if (axisLY > 0.25f || hatY > 0.5f) keyMask = keyMask or 8
        gamepadKeyMask = keyMask
        syncGamepadState()
        return true
    }

    private fun refreshGamepadConnected() {
        val connected = InputDevice.getDeviceIds().any { id ->
            val device = InputDevice.getDevice(id) ?: return@any false
            !device.isVirtual &&
                ((device.sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                    (device.sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
        }
        gamepadConnected = connected
        if (!connected) {
            gamepadKeyButtons = 0
            gamepadAxisButtons = 0
            gamepadKeyMask = 0
            axisLX = 0f
            axisLY = 0f
            axisRX = 0f
            axisRY = 0f
        }
        syncGamepadState()
    }

    private fun syncGamepadState() {
        nativeSetGamepadState(
            gamepadConnected,
            gamepadKeyButtons or gamepadAxisButtons,
            axisLX,
            axisLY,
            axisRX,
            axisRY
        )
    }

    private fun gamepadButtonBit(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> 1 shl 0
        KeyEvent.KEYCODE_BUTTON_B -> 1 shl 1
        KeyEvent.KEYCODE_BUTTON_X -> 1 shl 2
        KeyEvent.KEYCODE_BUTTON_Y -> 1 shl 3
        KeyEvent.KEYCODE_BUTTON_L1 -> 1 shl 4
        KeyEvent.KEYCODE_BUTTON_R1 -> 1 shl 5
        KeyEvent.KEYCODE_BUTTON_L2 -> 1 shl 6
        KeyEvent.KEYCODE_BUTTON_R2 -> 1 shl 7
        KeyEvent.KEYCODE_BUTTON_SELECT -> 1 shl 8
        KeyEvent.KEYCODE_BUTTON_START -> 1 shl 9
        KeyEvent.KEYCODE_BUTTON_THUMBL -> 1 shl 10
        KeyEvent.KEYCODE_BUTTON_THUMBR -> 1 shl 11
        KeyEvent.KEYCODE_DPAD_UP -> 1 shl 12
        KeyEvent.KEYCODE_DPAD_DOWN -> 1 shl 13
        KeyEvent.KEYCODE_DPAD_LEFT -> 1 shl 14
        KeyEvent.KEYCODE_DPAD_RIGHT -> 1 shl 15
        else -> 0
    }

    private fun gmlKeyCode(keyCode: Int): Int = when (keyCode) {
        in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> 112 + (keyCode - KeyEvent.KEYCODE_F1)
        in KeyEvent.KEYCODE_NUMPAD_0..KeyEvent.KEYCODE_NUMPAD_9 -> 96 + (keyCode - KeyEvent.KEYCODE_NUMPAD_0)
        in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 -> '0'.code + (keyCode - KeyEvent.KEYCODE_0)
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 'A'.code + (keyCode - KeyEvent.KEYCODE_A)
        KeyEvent.KEYCODE_DEL -> 8
        KeyEvent.KEYCODE_TAB -> 9
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> 13
        KeyEvent.KEYCODE_BREAK -> 19
        KeyEvent.KEYCODE_CAPS_LOCK -> 20
        KeyEvent.KEYCODE_ESCAPE -> 27
        KeyEvent.KEYCODE_SPACE -> 32
        KeyEvent.KEYCODE_PAGE_UP -> 33
        KeyEvent.KEYCODE_PAGE_DOWN -> 34
        KeyEvent.KEYCODE_MOVE_END -> 35
        KeyEvent.KEYCODE_MOVE_HOME -> 36
        KeyEvent.KEYCODE_DPAD_LEFT -> 37
        KeyEvent.KEYCODE_DPAD_UP -> 38
        KeyEvent.KEYCODE_DPAD_RIGHT -> 39
        KeyEvent.KEYCODE_DPAD_DOWN -> 40
        KeyEvent.KEYCODE_SYSRQ -> 44
        KeyEvent.KEYCODE_INSERT -> 45
        KeyEvent.KEYCODE_FORWARD_DEL -> 46
        KeyEvent.KEYCODE_NUMPAD_MULTIPLY -> 106
        KeyEvent.KEYCODE_NUMPAD_ADD -> 107
        KeyEvent.KEYCODE_NUMPAD_SUBTRACT -> 109
        KeyEvent.KEYCODE_NUMPAD_DOT -> 110
        KeyEvent.KEYCODE_NUMPAD_DIVIDE -> 111
        KeyEvent.KEYCODE_SHIFT_LEFT -> 160
        KeyEvent.KEYCODE_SHIFT_RIGHT -> 161
        KeyEvent.KEYCODE_CTRL_LEFT -> 162
        KeyEvent.KEYCODE_CTRL_RIGHT -> 163
        KeyEvent.KEYCODE_ALT_LEFT -> 164
        KeyEvent.KEYCODE_ALT_RIGHT -> 165
        KeyEvent.KEYCODE_SEMICOLON -> 186
        KeyEvent.KEYCODE_EQUALS -> 187
        KeyEvent.KEYCODE_COMMA -> 188
        KeyEvent.KEYCODE_MINUS -> 189
        KeyEvent.KEYCODE_PERIOD -> 190
        KeyEvent.KEYCODE_SLASH -> 191
        KeyEvent.KEYCODE_GRAVE -> 192
        KeyEvent.KEYCODE_LEFT_BRACKET -> 219
        KeyEvent.KEYCODE_BACKSLASH -> 220
        KeyEvent.KEYCODE_RIGHT_BRACKET -> 221
        KeyEvent.KEYCODE_APOSTROPHE -> 222
        else -> 0
    }

    fun stopGame() {
        running = false
        if (Thread.currentThread() !== renderThread) renderThread.join()
    }

    private fun renderLoop() {
        try {
            if (!nativeCreate(packageFile.absolutePath, saveDirectory.absolutePath)) error("Native game startup failed")
            val width = nativeWidth()
            val height = nativeHeight()
            val bitmap = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888)
            val pixels = IntArray(width * height)
            gameBitmap = bitmap
            while (running) {
                val start = System.nanoTime()
                nativeFrame(touchMask or physicalMask or gamepadKeyMask, pixels)
                bitmap.setPixels(pixels, 0, width, 0, 0, width, height)
                postInvalidateOnAnimation()
                val frameNanos = (1_000_000_000.0 / nativeSpeed().coerceIn(1f, 1000f)).toLong()
                val delay = frameNanos - (System.nanoTime() - start)
                if (delay > 0) Thread.sleep(delay / 1_000_000, (delay % 1_000_000).toInt())
            }
        } catch (error: Exception) {
            onFailure(error.message ?: "Game stopped")
        } finally {
            nativeDestroy()
        }
    }

    override fun onDraw(canvas: android.graphics.Canvas) {
        super.onDraw(canvas)
        val bitmap = gameBitmap ?: return
        val bounds = gameBounds(bitmap)
        canvas.drawBitmap(bitmap, null, bounds, paint)
        drawTouchControls(canvas)
    }

    override fun onTouchEvent(event: android.view.MotionEvent): Boolean {
        var mask = 0
        for (pointer in 0 until event.pointerCount) {
            if (isPointerReleased(event, pointer)) continue
            mask = mask or touchKey(event.getX(pointer), event.getY(pointer))
        }
        if (event.actionMasked == android.view.MotionEvent.ACTION_UP ||
            event.actionMasked == android.view.MotionEvent.ACTION_CANCEL) mask = 0
        touchMask = mask
        if (event.actionMasked == android.view.MotionEvent.ACTION_DOWN ||
            event.actionMasked == android.view.MotionEvent.ACTION_POINTER_DOWN) {
            val x = event.getX(event.actionIndex)
            val y = event.getY(event.actionIndex)
            if (keyboardButtonHit(x, y)) showKeyboard()
        }
        return true
    }

    override fun onCheckIsTextEditor() = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_ACTION_NONE or EditorInfo.IME_FLAG_NO_EXTRACT_UI
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                text?.forEach { character ->
                    val key = when {
                        character in 'a'..'z' -> character.uppercaseChar().code
                        character.code in 32..126 -> character.code
                        character == '\n' -> 13
                        else -> 0
                    }
                    if (key != 0) {
                        val keyCode = if (character == '\n') 13 else key
                        val textCode = if (character == '\n') 13 else character.code
                        typeKey(keyCode, textCode)
                    }
                }
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                if (beforeLength > 0) typeKey(8, 8)
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                if (event.action == KeyEvent.ACTION_DOWN) {
                    when (event.keyCode) {
                        KeyEvent.KEYCODE_DEL -> typeKey(8, 8)
                        KeyEvent.KEYCODE_ENTER -> typeKey(13, 13)
                    }
                }
                return true
            }
        }
    }

    private fun showKeyboard() {
        requestFocus()
        (context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager)
            .showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
    }

    private fun gameBounds(bitmap: android.graphics.Bitmap): android.graphics.RectF {
        val scale = minOf(width.toFloat() / bitmap.width, height.toFloat() / bitmap.height)
        val drawWidth = bitmap.width * scale
        val drawHeight = bitmap.height * scale
        val left = (width - drawWidth) / 2
        val top = (height - drawHeight) / 2
        return android.graphics.RectF(left, top, left + drawWidth, top + drawHeight)
    }

    private fun isPointerReleased(event: android.view.MotionEvent, pointer: Int): Boolean =
        (event.actionMasked == android.view.MotionEvent.ACTION_UP && pointer == 0) ||
            (event.actionMasked == android.view.MotionEvent.ACTION_POINTER_UP &&
                event.getPointerId(pointer) == event.getPointerId(event.actionIndex))

    private fun padGeometry(): Triple<Float, Float, Float> {
        val bitmap = gameBitmap ?: return Triple(0f, 0f, 0f)
        val bounds = gameBounds(bitmap)
        val side = bounds.left
        return Triple(side / 2, height * 0.74f, minOf(height * 0.045f, side / 5.2f))
    }

    private fun actionGeometry(): Triple<Float, Float, Float> {
        val bitmap = gameBitmap ?: return Triple(0f, 0f, 0f)
        val bounds = gameBounds(bitmap)
        val side = width - bounds.right
        return Triple(bounds.right + side / 2, height * 0.58f, minOf(height * 0.045f, side * 0.28f))
    }

    private fun touchKey(x: Float, y: Float): Int {
        val (padX, padY, padR) = padGeometry()
        val directions = listOf(
            Triple(padX - padR * 1.45f, padY, 1),
            Triple(padX, padY - padR * 1.45f, 2),
            Triple(padX + padR * 1.45f, padY, 4),
            Triple(padX, padY + padR * 1.45f, 8)
        )
        val direction = directions.firstOrNull { (cx, cy, _) ->
            kotlin.math.hypot((x - cx).toDouble(), (y - cy).toDouble()) <= padR * 1.3
        }?.third ?: 0
        val (actionX, actionY, actionR) = actionGeometry()
        val action = (0..2).firstOrNull { index ->
            val cy = actionY + index * height * 0.16f
            kotlin.math.hypot((x - actionX).toDouble(), (y - cy).toDouble()) <= actionR * 1.4
        }?.let { 1 shl (4 + it) } ?: 0
        return direction or action
    }

    private fun keyboardButtonHit(x: Float, y: Float): Boolean {
        val (actionX, _, actionR) = actionGeometry()
        val keyboardY = height * 0.22f
        return kotlin.math.hypot((x - actionX).toDouble(), (y - keyboardY).toDouble()) <= actionR * 1.5
    }

    private fun drawTouchControls(canvas: android.graphics.Canvas) {
        val controls = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
            color = 0x99505050.toInt()
            style = android.graphics.Paint.Style.FILL
        }
        val labels = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textAlign = android.graphics.Paint.Align.CENTER
            typeface = android.graphics.Typeface.DEFAULT_BOLD
        }
        val (padX, padY, padR) = padGeometry()
        val directions = listOf(
            Triple(padX - padR * 1.45f, padY, "◀"),
            Triple(padX, padY - padR * 1.45f, "▲"),
            Triple(padX + padR * 1.45f, padY, "▶"),
            Triple(padX, padY + padR * 1.45f, "▼")
        )
        directions.forEach { (x, y, label) ->
            canvas.drawCircle(x, y, padR, controls)
            labels.textSize = padR
            canvas.drawText(label, x, y + padR * 0.35f, labels)
        }
        val (actionX, actionY, actionR) = actionGeometry()
        listOf("Z", "X", "C").forEachIndexed { index, label ->
            val y = actionY + index * height * 0.16f
            canvas.drawCircle(actionX, y, actionR, controls)
            labels.textSize = actionR
            canvas.drawText(label, actionX, y + actionR * 0.35f, labels)
        }
        val keyboardY = height * 0.22f
        canvas.drawCircle(actionX, keyboardY, actionR, controls)
        labels.textSize = actionR * 0.56f
        canvas.drawText("ABC", actionX, keyboardY + actionR * 0.2f, labels)
    }

    companion object {
        init { System.loadLibrary("regml") }
    }
}
