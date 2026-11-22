package com.example.gnssble.ui.components

import android.content.Context
import android.graphics.BitmapFactory
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.opengl.GLUtils
import android.opengl.Matrix
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import com.example.gnssble.R
import com.example.gnssble.data.model.SatelliteOrbit
import com.example.gnssble.data.orbit.SatellitePropagator
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.nio.ShortBuffer
import kotlin.math.cos
import kotlin.math.sin

class EarthGlobeView(context: Context) : GLSurfaceView(context) {
    private val globeRenderer = EarthGlobeRenderer(context)
    private var lastX = 0f
    private val scaleDetector = ScaleGestureDetector(context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                globeRenderer.zoom(detector.scaleFactor)
                return true
            }
        },
    )

    init {
        setEGLContextClientVersion(2)
        setRenderer(globeRenderer)
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    fun update(orbits: List<SatelliteOrbit>, receiver: Pair<Double, Double>?, timestampMs: Long) {
        globeRenderer.update(orbits, receiver, timestampMs)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(event)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastX = event.x
                globeRenderer.beginDrag()
            }
            MotionEvent.ACTION_MOVE -> {
                if (event.pointerCount == 1 && !scaleDetector.isInProgress) {
                    val deltaX = event.x - lastX
                    lastX = event.x
                    // Match the screen gesture direction: dragging right rotates
                    // the visible globe and its markers to the right.
                    globeRenderer.rotate(-deltaX * 0.45f)
                }
            }
            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_CANCEL -> {
                globeRenderer.endDrag()
            }
        }
        return true
    }
}

private class EarthGlobeRenderer(private val context: Context) : GLSurfaceView.Renderer {
    private val projection = FloatArray(16)
    private val view = FloatArray(16)
    private val model = FloatArray(16)
    private val mvp = FloatArray(16)
    private var textureProgram = 0
    private var pointProgram = 0
    private var earthTexture = 0
    private var sphereVertices: FloatBuffer? = null
    private var sphereIndices: ShortBuffer? = null
    private var indexCount = 0
    private var width = 1
    private var height = 1
    @Volatile private var rotationDeg = 0f
    @Volatile private var zoomScale = 1f
    @Volatile private var dragTimestampMs: Long? = null
    @Volatile private var frame = GlobeFrame(emptyList(), null, System.currentTimeMillis())

    fun update(orbits: List<SatelliteOrbit>, receiver: Pair<Double, Double>?, timestampMs: Long) {
        frame = GlobeFrame(orbits, receiver, timestampMs)
    }

    fun rotate(deltaDeg: Float) {
        rotationDeg = (rotationDeg + deltaDeg).mod(360f)
    }

    fun zoom(scaleDelta: Float) {
        zoomScale = (zoomScale * scaleDelta).coerceIn(0.75f, 1.8f)
    }

    fun beginDrag() {
        dragTimestampMs = frame.timestampMs
    }

    fun endDrag() {
        dragTimestampMs = null
    }

    override fun onSurfaceCreated(gl: javax.microedition.khronos.opengles.GL10?, config: javax.microedition.khronos.egl.EGLConfig?) {
        GLES20.glClearColor(0.015f, 0.025f, 0.05f, 1f)
        GLES20.glEnable(GLES20.GL_DEPTH_TEST)
        GLES20.glEnable(GLES20.GL_CULL_FACE)
        textureProgram = createProgram(TEXTURE_VERTEX_SHADER, TEXTURE_FRAGMENT_SHADER)
        pointProgram = createProgram(POINT_VERTEX_SHADER, POINT_FRAGMENT_SHADER)
        createSphereMesh()
        earthTexture = loadTexture()
    }

    override fun onSurfaceChanged(gl: javax.microedition.khronos.opengles.GL10?, width: Int, height: Int) {
        this.width = width.coerceAtLeast(1)
        this.height = height.coerceAtLeast(1)
        GLES20.glViewport(0, 0, this.width, this.height)
        val aspect = this.width.toFloat() / this.height.toFloat()
        if (aspect >= 1f) {
            Matrix.orthoM(projection, 0, -aspect, aspect, -1f, 1f, 2f, 5f)
        } else {
            val inverseAspect = 1f / aspect
            Matrix.orthoM(projection, 0, -1f, 1f, -inverseAspect, inverseAspect, 2f, 5f)
        }
    }

    override fun onDrawFrame(gl: javax.microedition.khronos.opengles.GL10?) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)
        Matrix.setLookAtM(view, 0, 0f, 0f, 3.2f, 0f, 0f, 0f, 0f, 1f, 0f)
        Matrix.setIdentityM(model, 0)
        Matrix.rotateM(model, 0, EARTH_AXIAL_TILT_DEG, 0f, 0f, 1f)
        Matrix.rotateM(model, 0, rotationDeg, 0f, 1f, 0f)
        Matrix.scaleM(model, 0, zoomScale, zoomScale, zoomScale)
        Matrix.multiplyMM(mvp, 0, view, 0, model, 0)
        Matrix.multiplyMM(mvp, 0, projection, 0, mvp, 0)

        drawEarth()
        val markerFrame = frame.copy(timestampMs = dragTimestampMs ?: frame.timestampMs)
        drawMarkers(markerFrame)
    }

    private fun drawEarth() {
        val program = textureProgram
        GLES20.glUseProgram(program)
        val position = GLES20.glGetAttribLocation(program, "aPosition")
        val texCoord = GLES20.glGetAttribLocation(program, "aTexCoord")
        val matrix = GLES20.glGetUniformLocation(program, "uMvp")
        GLES20.glUniformMatrix4fv(matrix, 1, false, mvp, 0)
        sphereVertices?.position(0)
        GLES20.glVertexAttribPointer(position, 3, GLES20.GL_FLOAT, false, 20, sphereVertices)
        GLES20.glEnableVertexAttribArray(position)
        sphereVertices?.position(3)
        GLES20.glVertexAttribPointer(texCoord, 2, GLES20.GL_FLOAT, false, 20, sphereVertices)
        GLES20.glEnableVertexAttribArray(texCoord)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, earthTexture)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "uTexture"), 0)
        GLES20.glDrawElements(GLES20.GL_TRIANGLES, indexCount, GLES20.GL_UNSIGNED_SHORT, sphereIndices)
        GLES20.glDisableVertexAttribArray(position)
        GLES20.glDisableVertexAttribArray(texCoord)
    }

    private fun drawMarkers(currentFrame: GlobeFrame) {
        val positions = buildList {
            currentFrame.orbits.forEach { orbit ->
                val satellite = SatellitePropagator.propagate(orbit, currentFrame.timestampMs)
                add(floatArrayOf(satellite.latitudeDeg.toFloat(), satellite.longitudeDeg.toFloat(), 1f))
            }
            currentFrame.receiver?.let { (lat, lon) -> add(floatArrayOf(lat.toFloat(), lon.toFloat(), 2f)) }
        }
        if (positions.isEmpty()) return
        val buffer = ByteBuffer.allocateDirect(positions.size * 3 * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer()
        positions.forEach(buffer::put)
        buffer.position(0)

        val program = pointProgram
        // Catalog markers remain visible while zooming, including satellites
        // on the far side of the globe. This prevents the catalog from
        // appearing to lose objects as the earth is enlarged.
        GLES20.glDisable(GLES20.GL_DEPTH_TEST)
        GLES20.glUseProgram(program)
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "uMvp"), 1, false, mvp, 0)
        val position = GLES20.glGetAttribLocation(program, "aLatLonType")
        GLES20.glVertexAttribPointer(position, 3, GLES20.GL_FLOAT, false, 12, buffer)
        GLES20.glEnableVertexAttribArray(position)
        GLES20.glDrawArrays(GLES20.GL_POINTS, 0, positions.size)
        GLES20.glDisableVertexAttribArray(position)
        GLES20.glEnable(GLES20.GL_DEPTH_TEST)
    }

    private fun createSphereMesh() {
        val latSegments = 72
        val lonSegments = 144
        val vertices = ArrayList<Float>((latSegments + 1) * (lonSegments + 1) * 5)
        for (latIndex in 0..latSegments) {
            val latitude = Math.PI / 2.0 - latIndex * Math.PI / latSegments
            for (lonIndex in 0..lonSegments) {
                val longitude = lonIndex * 2.0 * Math.PI / lonSegments
                vertices += (cos(latitude) * cos(longitude)).toFloat()
                vertices += sin(latitude).toFloat()
                vertices += (cos(latitude) * sin(longitude)).toFloat()
                // Keep the texture longitude in the same direction as the
                // sphere vertices and satellite marker coordinates. Reversing
                // this value mirrors the earth beneath the markers, making
                // them appear to travel in opposite directions while dragging.
                vertices += (lonIndex.toDouble() / lonSegments).toFloat()
                vertices += (latIndex.toDouble() / latSegments).toFloat()
            }
        }
        val indices = ArrayList<Short>(latSegments * lonSegments * 6)
        for (latIndex in 0 until latSegments) {
            for (lonIndex in 0 until lonSegments) {
                val first = (latIndex * (lonSegments + 1) + lonIndex).toShort()
                val second = (first + lonSegments + 1).toShort()
                indices += first; indices += second; indices += (first + 1).toShort()
                indices += second; indices += (second + 1).toShort(); indices += (first + 1).toShort()
            }
        }
        sphereVertices = floatBuffer(vertices.toFloatArray())
        sphereIndices = shortBuffer(indices.toShortArray())
        indexCount = indices.size
    }

    private fun loadTexture(): Int {
        val ids = IntArray(1)
        GLES20.glGenTextures(1, ids, 0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, ids[0])
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        val bitmap = BitmapFactory.decodeResource(context.resources, R.drawable.earth_blue_marble)
        GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0)
        bitmap.recycle()
        return ids[0]
    }

    private fun createProgram(vertexSource: String, fragmentSource: String): Int {
        fun compile(type: Int, source: String): Int {
            val shader = GLES20.glCreateShader(type)
            GLES20.glShaderSource(shader, source)
            GLES20.glCompileShader(shader)
            return shader
        }
        val program = GLES20.glCreateProgram()
        GLES20.glAttachShader(program, compile(GLES20.GL_VERTEX_SHADER, vertexSource))
        GLES20.glAttachShader(program, compile(GLES20.GL_FRAGMENT_SHADER, fragmentSource))
        GLES20.glLinkProgram(program)
        return program
    }

    private fun floatBuffer(values: FloatArray): FloatBuffer = ByteBuffer.allocateDirect(values.size * 4)
        .order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(values); position(0) }

    private fun shortBuffer(values: ShortArray): ShortBuffer = ByteBuffer.allocateDirect(values.size * 2)
        .order(ByteOrder.nativeOrder()).asShortBuffer().apply { put(values); position(0) }

    private data class GlobeFrame(
        val orbits: List<SatelliteOrbit>,
        val receiver: Pair<Double, Double>?,
        val timestampMs: Long,
    )

    private companion object {
        const val EARTH_AXIAL_TILT_DEG = 23.44f
        const val TEXTURE_VERTEX_SHADER = """
            attribute vec3 aPosition;
            attribute vec2 aTexCoord;
            uniform mat4 uMvp;
            varying vec2 vTexCoord;
            void main() { gl_Position = uMvp * vec4(aPosition, 1.0); vTexCoord = aTexCoord; }
        """
        const val TEXTURE_FRAGMENT_SHADER = """
            precision mediump float;
            uniform sampler2D uTexture;
            varying vec2 vTexCoord;
            void main() { gl_FragColor = texture2D(uTexture, vTexCoord); }
        """
        const val POINT_VERTEX_SHADER = """
            attribute vec3 aLatLonType;
            uniform mat4 uMvp;
            void main() {
                float lat = radians(aLatLonType.x);
                float lon = radians(aLatLonType.y);
                vec3 p = vec3(cos(lat) * cos(lon), sin(lat), cos(lat) * sin(lon));
                gl_Position = uMvp * vec4(p * 1.035, 1.0);
                gl_PointSize = aLatLonType.z > 1.5 ? 14.0 : 9.0;
            }
        """
        const val POINT_FRAGMENT_SHADER = """
            precision mediump float;
            void main() {
                vec2 c = gl_PointCoord - vec2(0.5);
                if (dot(c, c) > 0.25) discard;
                gl_FragColor = vec4(1.0, 0.83, 0.20, 1.0);
            }
        """
    }
}
