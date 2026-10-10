package com.nfsmw.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * The on-screen gamepad: a full Xbox 360 pad (both sticks, triggers, bumpers, D-pad, A/B/X/Y, Back and Start)
 * that the player can rearrange.
 *
 * Positions are stored as fractions of the view (centre x of the width, centre y of the height) and sizes as a
 * fraction of the height, so a layout keeps its shape on any screen. The state goes to the SDK's touch pad
 * (rex_sdl_set_touch_gamepad_state) only when it changes, and the view redraws only then.
 */
public final class TouchControlsView extends View {
    // XInput button bits (X_INPUT_GAMEPAD_*).
    static final int DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008;
    static final int START = 0x0010, BACK = 0x0020, LTHUMB = 0x0040, RTHUMB = 0x0080;
    static final int LB = 0x0100, RB = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;

    private static final int BUTTON = 0, STICK = 1, DPAD = 2, PEDAL = 3, MENU = 4;
    private static final int TRIGGER_NONE = 0, TRIGGER_LEFT = 1, TRIGGER_RIGHT = 2;
    private static final String PREFS = "nfsmw_controls";

    static native void nativeSetTouchState(int buttons, int leftX, int leftY, int rightX, int rightY,
                                           int leftTrigger, int rightTrigger);

    /** One control of the pad. */
    static final class Control {
        final String id;
        final int type;
        final String label;
        final String hint;
        final int mask;
        final int trigger;
        final int color;
        final float defX, defY, defSize;
        final boolean defVisible;
        float x, y, size;
        boolean visible;

        // Play state.
        boolean pressed;
        int dpad;                  // D-pad bits pressed
        int pointer = -1;          // stick: the finger that holds it
        float originX, originY;    // stick: where that finger went down
        float knobX, knobY;        // stick: -1..1

        Control(String id, int type, String label, String hint, int mask, int trigger, int color,
                float x, float y, float size, boolean visible) {
            this.id = id;
            this.type = type;
            this.label = label;
            this.hint = hint;
            this.mask = mask;
            this.trigger = trigger;
            this.color = color;
            this.defX = x;
            this.defY = y;
            this.defSize = size;
            this.defVisible = visible;
            reset();
        }

        void reset() {
            x = defX;
            y = defY;
            size = defSize;
            visible = defVisible;
        }
    }

    /** What the activity shows around the pad: the editor toolbar and the settings dialog. */
    interface Host {
        void onEditModeChanged(boolean editing);
    }

    private final List<Control> controls = new ArrayList<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dashed = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final SharedPreferences prefs;
    private Host host;
    private final String hiddenLabel;
    private final String tiltLabel;

    // Settings.
    float opacity = 0.75f;
    boolean haptics = true;
    boolean tiltSteering = false;
    float tiltSensitivity = 1.0f;   // 1 = full lock at 30 degrees
    boolean hideWithGamepad = true;
    boolean stretch = true;

    // State.
    private boolean editing;
    private boolean hiddenByGamepad;
    private Control selected;
    private float dragDX, dragDY;
    private float pinchStart, sizeAtPinch;
    private float tilt;             // -1..1 from the gravity sensor
    private int lastButtons = -1, lastLX, lastLY, lastRX, lastRY, lastLT = -1, lastRT = -1;

    public TouchControlsView(Context context) {
        super(context);
        prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        hiddenLabel = context.getString(R.string.control_hidden);
        tiltLabel = context.getString(R.string.control_tilt);
        setClickable(true);
        setHapticFeedbackEnabled(true);
        text.setTypeface(Typeface.DEFAULT_BOLD);
        text.setTextAlign(Paint.Align.CENTER);
        stroke.setStyle(Paint.Style.STROKE);
        dashed.setStyle(Paint.Style.STROKE);
        dashed.setPathEffect(new DashPathEffect(new float[] {14f, 10f}, 0f));
        createControls();
        load();
    }

    void setHost(Host host) {
        this.host = host;
    }

    // ---- Layout ------------------------------------------------------------------------------------------

    private void createControls() {
        final int grey = 0xFF2A3440;
        // Left hand: steering stick, D-pad, LB.
        add(new Control("ls", STICK, "", "DIRECCION", 0, TRIGGER_NONE, grey, .15f, .70f, .40f, true));
        add(new Control("dpad", DPAD, "", "", 0, TRIGGER_NONE, grey, .10f, .30f, .27f, true));
        add(new Control("lb", BUTTON, "LB", "", LB, TRIGGER_NONE, grey, .07f, .09f, .13f, true));
        // Right hand: pedals, A/B/X/Y diamond, RB.
        add(new Control("rt", PEDAL, "GAS", "RT", 0, TRIGGER_RIGHT, 0xFF1E5E3A, .92f, .70f, .42f, true));
        add(new Control("lt", PEDAL, "FRENO", "LT", 0, TRIGGER_LEFT, 0xFF6B2323, .80f, .76f, .32f, true));
        add(new Control("a", BUTTON, "A", "MANO", A, TRIGGER_NONE, 0xFF2E7D32, .78f, .45f, .15f, true));
        add(new Control("b", BUTTON, "B", "NITRO", B, TRIGGER_NONE, 0xFFC62828, .87f, .36f, .15f, true));
        add(new Control("x", BUTTON, "X", "CAMARA", X, TRIGGER_NONE, 0xFF1565C0, .69f, .36f, .15f, true));
        add(new Control("y", BUTTON, "Y", "MIRAR", Y, TRIGGER_NONE, 0xFFF9A825, .78f, .27f, .15f, true));
        add(new Control("rb", BUTTON, "RB", "", RB, TRIGGER_NONE, grey, .93f, .09f, .13f, true));
        // Middle: Back, Start and the editor.
        add(new Control("back", BUTTON, "BACK", "", BACK, TRIGGER_NONE, grey, .37f, .08f, .10f, true));
        add(new Control("menu", MENU, "⚙", "", 0, TRIGGER_NONE, grey, .50f, .08f, .09f, true));
        add(new Control("start", BUTTON, "START", "", START, TRIGGER_NONE, grey, .63f, .08f, .10f, true));
        // Off by default: the camera stick and the stick clicks.
        add(new Control("rs", STICK, "", "CAMARA", 0, TRIGGER_NONE, grey, .55f, .72f, .30f, false));
        add(new Control("l3", BUTTON, "L3", "", LTHUMB, TRIGGER_NONE, grey, .28f, .88f, .11f, false));
        add(new Control("r3", BUTTON, "R3", "", RTHUMB, TRIGGER_NONE, grey, .45f, .88f, .11f, false));
    }

    private void add(Control c) {
        controls.add(c);
    }

    private void load() {
        opacity = prefs.getFloat("opacity", opacity);
        haptics = prefs.getBoolean("haptics", haptics);
        tiltSteering = prefs.getBoolean("tilt", tiltSteering);
        tiltSensitivity = prefs.getFloat("tilt_sensitivity", tiltSensitivity);
        hideWithGamepad = prefs.getBoolean("hide_with_gamepad", hideWithGamepad);
        stretch = prefs.getBoolean("stretch", stretch);
        String json = prefs.getString("layout", null);
        if (json == null) {
            return;
        }
        try {
            JSONObject all = new JSONObject(json);
            for (Control c : controls) {
                JSONObject o = all.optJSONObject(c.id);
                if (o != null) {
                    c.x = clamp((float) o.optDouble("x", c.defX), 0f, 1f);
                    c.y = clamp((float) o.optDouble("y", c.defY), 0f, 1f);
                    c.size = clamp((float) o.optDouble("s", c.defSize), .06f, .8f);
                    c.visible = o.optBoolean("v", c.defVisible);
                }
            }
        } catch (JSONException ignored) {
            // A damaged layout: keep the defaults.
        }
    }

    void save() {
        JSONObject all = new JSONObject();
        try {
            for (Control c : controls) {
                JSONObject o = new JSONObject();
                o.put("x", c.x).put("y", c.y).put("s", c.size).put("v", c.visible);
                all.put(c.id, o);
            }
        } catch (JSONException ignored) {
            return;
        }
        prefs.edit()
                .putString("layout", all.toString())
                .putFloat("opacity", opacity)
                .putBoolean("haptics", haptics)
                .putBoolean("tilt", tiltSteering)
                .putFloat("tilt_sensitivity", tiltSensitivity)
                .putBoolean("hide_with_gamepad", hideWithGamepad)
                .putBoolean("stretch", stretch)
                .apply();
    }

    void resetLayout() {
        for (Control c : controls) {
            c.reset();
        }
        selected = null;
        invalidate();
    }

    // ---- Editor ------------------------------------------------------------------------------------------

    boolean isEditing() {
        return editing;
    }

    void setEditing(boolean on) {
        if (editing == on) {
            return;
        }
        editing = on;
        selected = null;
        releaseAll();
        if (!on) {
            save();
        }
        if (host != null) {
            host.onEditModeChanged(on);
        }
        invalidate();
    }

    /** The control the editor toolbar acts on, or null. */
    Control selected() {
        return selected;
    }

    void resizeSelected(float factor) {
        if (selected != null) {
            selected.size = clamp(selected.size * factor, .06f, .8f);
            invalidate();
        }
    }

    void toggleSelectedVisible() {
        if (selected != null && selected.type != MENU) {
            selected.visible = !selected.visible;
            invalidate();
        }
    }

    // ---- Gamepad and sensors -----------------------------------------------------------------------------

    /** A physical gamepad was used or connected: the touch pad gets out of the way. */
    void onGamepadActive() {
        if (hideWithGamepad && !hiddenByGamepad && !editing) {
            hiddenByGamepad = true;
            releaseAll();
            invalidate();
        }
    }

    /** The last physical gamepad went away. */
    void onGamepadGone() {
        if (hiddenByGamepad) {
            hiddenByGamepad = false;
            invalidate();
        }
    }

    /** Tilt from the gravity sensor, -1 (left) to 1 (right), already corrected for the screen rotation. */
    void setTilt(float value) {
        tilt = value;
        if (tiltSteering && !editing && !hiddenByGamepad) {
            send();
        }
    }

    // ---- Input -------------------------------------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (editing) {
            return editTouch(event);
        }
        if (hiddenByGamepad) {
            // A touch brings the pad back.
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                hiddenByGamepad = false;
                invalidate();
            }
            return true;
        }
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            return true;
        }
        final int liftedIndex = (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
                ? event.getActionIndex() : -1;
        final int buttonsBefore = buttonState();

        // Sticks hold on to the finger that grabbed them.
        for (Control c : controls) {
            if (c.type != STICK || c.pointer < 0) {
                continue;
            }
            int index = event.findPointerIndex(c.pointer);
            if (index < 0 || index == liftedIndex) {
                c.pointer = -1;
                c.knobX = c.knobY = 0f;
                continue;
            }
            float radius = radius(c);
            c.knobX = clamp((event.getX(index) - c.originX) / radius, -1f, 1f);
            c.knobY = clamp((event.getY(index) - c.originY) / radius, -1f, 1f);
            float length = (float) Math.hypot(c.knobX, c.knobY);
            if (length > 1f) {
                c.knobX /= length;
                c.knobY /= length;
            }
        }
        // A new finger on a stick grabs it; the stick centres itself under the finger.
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int index = event.getActionIndex();
            float px = event.getX(index), py = event.getY(index);
            Control stick = hitAny(px, py);
            if (stick != null && stick.type == STICK && stick.pointer < 0) {
                stick.pointer = event.getPointerId(index);
                stick.originX = px;
                stick.originY = py;
                stick.knobX = stick.knobY = 0f;
            }
        }
        // Buttons, pedals and the D-pad follow the fingers, so a thumb can slide from gas to brake.
        for (Control c : controls) {
            c.pressed = false;
            c.dpad = 0;
        }
        for (int i = 0; i < event.getPointerCount(); i++) {
            if (i == liftedIndex || holdsStick(event.getPointerId(i))) {
                continue;
            }
            float px = event.getX(i), py = event.getY(i);
            Control c = hitAny(px, py);
            if (c == null || c.type == STICK) {
                continue;
            }
            if (c.type == DPAD) {
                c.dpad |= dpadBits(c, px, py);
            } else {
                c.pressed = true;
            }
        }
        // The gear opens the editor when the finger lifts on it.
        if (liftedIndex >= 0 && hitAny(event.getX(liftedIndex), event.getY(liftedIndex)) == menuControl()) {
            releaseAll();
            setEditing(true);
            return true;
        }
        final int buttonsAfter = buttonState();
        if (haptics && (buttonsAfter & ~buttonsBefore) != 0) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        }
        send();
        return true;
    }

    private boolean editTouch(MotionEvent event) {
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            selected = hitAny(event.getX(), event.getY(), true);
            if (selected != null) {
                dragDX = selected.x * getWidth() - event.getX();
                dragDY = selected.y * getHeight() - event.getY();
            }
            invalidate();
        } else if (action == MotionEvent.ACTION_POINTER_DOWN && selected != null && event.getPointerCount() == 2) {
            pinchStart = spacing(event);
            sizeAtPinch = selected.size;
        } else if (action == MotionEvent.ACTION_MOVE && selected != null) {
            if (event.getPointerCount() >= 2 && pinchStart > 0f) {
                selected.size = clamp(sizeAtPinch * spacing(event) / pinchStart, .06f, .8f);
            } else if (event.getPointerCount() == 1) {
                selected.x = clamp((event.getX() + dragDX) / getWidth(), 0f, 1f);
                selected.y = clamp((event.getY() + dragDY) / getHeight(), 0f, 1f);
            }
            invalidate();
        } else if (action == MotionEvent.ACTION_POINTER_UP) {
            pinchStart = 0f;
            if (selected != null) {
                // The finger left on screen keeps dragging from where it is now.
                int keep = event.getActionIndex() == 0 ? 1 : 0;
                dragDX = selected.x * getWidth() - event.getX(keep);
                dragDY = selected.y * getHeight() - event.getY(keep);
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            pinchStart = 0f;
        }
        return true;
    }

    private static float spacing(MotionEvent e) {
        return (float) Math.hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1));
    }

    private boolean holdsStick(int pointerId) {
        for (Control c : controls) {
            if (c.type == STICK && c.pointer == pointerId) {
                return true;
            }
        }
        return false;
    }

    private Control menuControl() {
        for (Control c : controls) {
            if (c.type == MENU) {
                return c;
            }
        }
        return null;
    }

    private int dpadBits(Control c, float px, float py) {
        float dx = px - c.x * getWidth(), dy = py - c.y * getHeight();
        if (Math.hypot(dx, dy) < radius(c) * .2f) {
            return 0;
        }
        double angle = Math.toDegrees(Math.atan2(-dy, dx));  // 0 = right, 90 = up
        if (angle < 0) {
            angle += 360;
        }
        int bits = 0;
        // 67.5-degree sectors per direction: diagonals press two.
        if (angle < 67.5 || angle > 292.5) bits |= DPAD_RIGHT;
        if (angle > 22.5 && angle < 157.5) bits |= DPAD_UP;
        if (angle > 112.5 && angle < 247.5) bits |= DPAD_LEFT;
        if (angle > 202.5 && angle < 337.5) bits |= DPAD_DOWN;
        return bits;
    }

    private Control hitAny(float px, float py) {
        return hitAny(px, py, false);
    }

    /** The control under a point; the nearest one when several overlap. In the editor, hidden ones too. */
    private Control hitAny(float px, float py, boolean includeHidden) {
        Control best = null;
        double bestDistance = Double.MAX_VALUE;
        for (Control c : controls) {
            if ((!c.visible && !includeHidden) || !contains(c, px, py, c.type == STICK ? 1.15f : 1.08f)) {
                continue;
            }
            double d = Math.hypot(px - c.x * getWidth(), py - c.y * getHeight()) / radius(c);
            if (d < bestDistance) {
                bestDistance = d;
                best = c;
            }
        }
        return best;
    }

    private boolean contains(Control c, float px, float py, float slack) {
        float cx = c.x * getWidth(), cy = c.y * getHeight();
        if (c.type == PEDAL) {
            float halfW = pedalWidth(c) * .5f * slack, halfH = c.size * getHeight() * .5f * slack;
            return Math.abs(px - cx) <= halfW && Math.abs(py - cy) <= halfH;
        }
        return Math.hypot(px - cx, py - cy) <= radius(c) * slack;
    }

    private float radius(Control c) {
        return c.size * getHeight() * .5f;
    }

    private float pedalWidth(Control c) {
        return c.size * getHeight() * .55f;
    }

    private void releaseAll() {
        for (Control c : controls) {
            c.pressed = false;
            c.dpad = 0;
            c.pointer = -1;
            c.knobX = c.knobY = 0f;
        }
        send();
        invalidate();
    }

    private int buttonState() {
        int buttons = 0;
        for (Control c : controls) {
            if (c.type == BUTTON && c.pressed) {
                buttons |= c.mask;
            } else if (c.type == DPAD) {
                buttons |= c.dpad;
            }
        }
        return buttons;
    }

    /** Sends the pad to the game if it changed, and redraws. */
    private void send() {
        boolean active = !editing && !hiddenByGamepad;
        int buttons = active ? buttonState() : 0;
        int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        boolean steeringHeld = false;
        if (active) {
            for (Control c : controls) {
                if (c.type == STICK && c.id.equals("ls")) {
                    steeringHeld = c.pointer >= 0;
                    lx = axis(c.knobX);
                    ly = axis(-c.knobY);  // XInput: up is positive
                } else if (c.type == STICK) {
                    rx = axis(c.knobX);
                    ry = axis(-c.knobY);
                } else if (c.type == PEDAL && c.pressed) {
                    if (c.trigger == TRIGGER_LEFT) lt = 255;
                    else rt = 255;
                }
            }
            // Touch steering wins while held; releasing it resumes tilt steering.
            if (tiltSteering && !steeringHeld) {
                lx = axis(clamp(tilt * tiltSensitivity, -1f, 1f));
            }
        }
        if (buttons == lastButtons && lx == lastLX && ly == lastLY && rx == lastRX && ry == lastRY &&
                lt == lastLT && rt == lastRT) {
            return;
        }
        lastButtons = buttons;
        lastLX = lx;
        lastLY = ly;
        lastRX = rx;
        lastRY = ry;
        lastLT = lt;
        lastRT = rt;
        nativeSetTouchState(buttons, lx, ly, rx, ry, lt, rt);
        invalidate();
    }

    private static int axis(float v) {
        return Math.round(clamp(v, -1f, 1f) * 32767f);
    }

    private static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // ---- Drawing -----------------------------------------------------------------------------------------

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (hiddenByGamepad && !editing) {
            return;
        }
        if (editing) {
            canvas.drawColor(0x66000000);
        }
        for (Control c : controls) {
            if (!c.visible && !editing) {
                continue;
            }
            float alpha = c.visible ? (editing ? 1f : opacity) : .35f;
            drawControl(canvas, c, alpha);
            if (editing) {
                float cx = c.x * getWidth(), cy = c.y * getHeight();
                dashed.setStrokeWidth(3f);
                dashed.setColor(c == selected ? 0xFFFFC107 : 0x88FFFFFF);
                if (c.type == PEDAL) {
                    float hw = pedalWidth(c) * .5f + 8f, hh = c.size * getHeight() * .5f + 8f;
                    canvas.drawRect(cx - hw, cy - hh, cx + hw, cy + hh, dashed);
                } else {
                    canvas.drawCircle(cx, cy, radius(c) + 8f, dashed);
                }
                if (!c.visible) {
                    text.setColor(0xFFFFFFFF);
                    text.setTextSize(Math.max(22f, radius(c) * .28f));
                    canvas.drawText(hiddenLabel, cx, cy + radius(c) + text.getTextSize() + 6f, text);
                }
            }
        }
    }

    private void drawControl(Canvas canvas, Control c, float alpha) {
        final float cx = c.x * getWidth(), cy = c.y * getHeight(), r = radius(c);
        final int a = Math.round(alpha * 255f);
        switch (c.type) {
            case STICK: {
                boolean held = c.pointer >= 0;
                float bx = held ? c.originX : cx, by = held ? c.originY : cy;
                boolean tiltMode = c.id.equals("ls") && tiltSteering && !editing && !held;
                fill.setColor(withAlpha(0xFF101820, a * 110 / 255));
                canvas.drawCircle(bx, by, r, fill);
                stroke.setStrokeWidth(Math.max(2f, r * .03f));
                stroke.setColor(withAlpha(0xFFEAF2FA, a * 160 / 255));
                canvas.drawCircle(bx, by, r, stroke);
                float knobR = r * .42f;
                float kx = bx + (tiltMode ? clamp(tilt * tiltSensitivity, -1f, 1f) : c.knobX) * (r - knobR);
                float ky = by + (tiltMode ? 0f : c.knobY) * (r - knobR);
                fill.setColor(withAlpha(held ? 0xFFFF9A32 : 0xFF3A4654, a * 220 / 255));
                canvas.drawCircle(kx, ky, knobR, fill);
                canvas.drawCircle(kx, ky, knobR, stroke);
                label(canvas, tiltMode ? tiltLabel : c.hint, bx, by + r + r * .22f, r * .16f, a);
                break;
            }
            case DPAD: {
                float arm = r * .36f;
                fill.setColor(withAlpha(0xFF101820, a * 110 / 255));
                canvas.drawCircle(cx, cy, r, fill);
                stroke.setStrokeWidth(Math.max(2f, r * .03f));
                stroke.setColor(withAlpha(0xFFEAF2FA, a * 160 / 255));
                canvas.drawCircle(cx, cy, r, stroke);
                drawArrow(canvas, cx, cy - r * .6f, arm, 0, (c.dpad & DPAD_UP) != 0, a);
                drawArrow(canvas, cx, cy + r * .6f, arm, 180, (c.dpad & DPAD_DOWN) != 0, a);
                drawArrow(canvas, cx - r * .6f, cy, arm, 270, (c.dpad & DPAD_LEFT) != 0, a);
                drawArrow(canvas, cx + r * .6f, cy, arm, 90, (c.dpad & DPAD_RIGHT) != 0, a);
                break;
            }
            case PEDAL: {
                float hw = pedalWidth(c) * .5f, hh = c.size * getHeight() * .5f;
                rect.set(cx - hw, cy - hh, cx + hw, cy + hh);
                float corner = hw * .35f;
                fill.setColor(withAlpha(c.pressed ? 0xFFFF9A32 : c.color, a * 200 / 255));
                canvas.drawRoundRect(rect, corner, corner, fill);
                stroke.setStrokeWidth(Math.max(2f, hw * .05f));
                stroke.setColor(withAlpha(c.pressed ? 0xFFFFD18A : 0xFFEAF2FA, a * 190 / 255));
                canvas.drawRoundRect(rect, corner, corner, stroke);
                // Grip lines, like a pedal.
                stroke.setStrokeWidth(Math.max(2f, hw * .04f));
                for (int i = -2; i <= 2; i++) {
                    float ly = cy + i * hh * .16f + hh * .25f;
                    canvas.drawLine(cx - hw * .55f, ly, cx + hw * .55f, ly, stroke);
                }
                label(canvas, c.label, cx, cy - hh * .45f, hw * .36f, a);
                label(canvas, c.hint, cx, cy - hh * .45f + hw * .42f, hw * .22f, a);
                break;
            }
            default: {
                fill.setColor(withAlpha(0xFF000000, a * 80 / 255));
                canvas.drawCircle(cx, cy + r * .06f, r * 1.02f, fill);
                fill.setColor(withAlpha(c.pressed ? 0xFFFF9A32 : 0xFF18232E, a * 190 / 255));
                canvas.drawCircle(cx, cy, r, fill);
                stroke.setStrokeWidth(Math.max(2.5f, r * .08f));
                stroke.setColor(withAlpha(c.pressed ? 0xFFFFD18A : c.color == 0xFF2A3440 ? 0xFFEAF2FA : c.color,
                        a * 230 / 255));
                canvas.drawCircle(cx, cy, r - stroke.getStrokeWidth() * .5f, stroke);
                boolean shortLabel = c.label.length() <= 2;
                float main = shortLabel ? r * (c.label.length() == 1 ? .85f : .6f) : r * .42f;
                if (c.hint.isEmpty()) {
                    label(canvas, c.label, cx, cy + main * .36f, main, a);
                } else {
                    label(canvas, c.label, cx, cy + main * .12f, main, a);
                    label(canvas, c.hint, cx, cy + r * .62f, r * .26f, a);
                }
                break;
            }
        }
    }

    private void drawArrow(Canvas canvas, float x, float y, float size, float rotation, boolean on, int a) {
        canvas.save();
        canvas.rotate(rotation, x, y);
        fill.setColor(withAlpha(on ? 0xFFFF9A32 : 0xFFEAF2FA, a * (on ? 240 : 170) / 255));
        android.graphics.Path p = new android.graphics.Path();
        p.moveTo(x, y - size * .5f);
        p.lineTo(x + size * .5f, y + size * .35f);
        p.lineTo(x - size * .5f, y + size * .35f);
        p.close();
        canvas.drawPath(p, fill);
        canvas.restore();
    }

    private void label(Canvas canvas, String s, float x, float baseline, float size, int a) {
        if (s == null || s.isEmpty()) {
            return;
        }
        text.setTextSize(size);
        text.setColor(withAlpha(0xFFFFFFFF, a));
        text.setShadowLayer(size * .15f, 0f, size * .05f, withAlpha(0xFF000000, a));
        canvas.drawText(s, x, baseline, text);
        text.clearShadowLayer();
    }

    private static int withAlpha(int color, int alpha) {
        return (color & 0x00FFFFFF) | (Math.max(0, Math.min(255, alpha)) << 24);
    }
}
