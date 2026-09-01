/* RAINFALL -- a decaying twinkle.
 *
 * QMK's stock PIXEL_RAIN does NOT fade: it sets one LED per interval (half the
 * time to black) and leaves every other LED lit at full until it happens to be
 * picked again. The result is a mostly-static scatter, not rain. STARLIGHT and
 * STARLIGHT_SMOOTH are no closer -- both keep the entire board lit, pulsing on
 * a shared or per-LED sine.
 *
 * This one keeps a brightness and a hue per LED. Every frame each brightness
 * decays; on a timer a random LED is seeded to full with a random hue. So a key
 * flashes and falls away over about a second, and only the few most recent
 * drops are anywhere near full.
 *
 * SPEED sets the DROP RATE, i.e. the density. Decay is deliberately fixed, so
 * turning speed up adds raindrops rather than making each one shorter -- which
 * is the knob you actually want when the effect looks too sparse or too busy.
 *
 * Cost: 2 bytes per LED (162 B here) and one qsub8 per LED per frame.
 */
RGB_MATRIX_EFFECT(RAINFALL)

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS

/* ~1.4 s from full to dark at the ~62 Hz effect rate (255/4 frames). Long
 * enough to read as a fall rather than a blink. */
#    define RAINFALL_DECAY 4

static uint8_t rainfall_v[RGB_MATRIX_LED_COUNT];
static uint8_t rainfall_h[RGB_MATRIX_LED_COUNT];

static bool RAINFALL(effect_params_t* params) {
    static uint8_t countdown = 0;

    if (params->init) {
        memset(rainfall_v, 0, sizeof(rainfall_v));
        countdown = 0;
    }

    RGB_MATRIX_USE_LIMITS(led_min, led_max);

    /* The effect is called once per LED_PROCESS_LIMIT chunk (5 chunks per frame
     * here), so the simulation must advance on the FIRST chunk only -- stepping
     * it per chunk would decay five times a frame and quintuple the drop rate. */
    if (params->iter == 0) {
        for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++)
            rainfall_v[i] = qsub8(rainfall_v[i], RAINFALL_DECAY);

        if (countdown) countdown--;
        if (!countdown) {
            uint8_t i     = random8_max(RGB_MATRIX_LED_COUNT);
            rainfall_v[i] = 255;
            rainfall_h[i] = random8();
            /* Frames between drops: 25 at speed 0 down to 1 at full. At the
             * default speed this leaves roughly half a dozen lit at once. */
            countdown = 1 + ((255 - rgb_matrix_config.speed) * 24) / 255;
        }
    }

    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();
        hsv_t hsv = {rainfall_h[i], rgb_matrix_config.hsv.s,
                     scale8(rainfall_v[i], rgb_matrix_config.hsv.v)};
        rgb_t rgb = rgb_matrix_hsv_to_rgb(hsv);
        rgb_matrix_set_color(i, rgb.r, rgb.g, rgb.b);
    }
    return rgb_matrix_check_finished_leds(led_max);
}

#endif // RGB_MATRIX_CUSTOM_EFFECT_IMPLS


/* DRIFT -- a scrolling 2D value-noise field on a hand-picked colour ramp.
 *
 * The noise controls POSITION ALONG A RAMP, never hue directly. Mapping a
 * scalar onto hue is what makes every stock effect read as "RGB keyboard"; this
 * one only ever wears colours that were chosen deliberately.
 *
 * Value noise rather than Perlin: Perlin needs gradient vectors and dot
 * products, value noise needs a table and three lerps, and at 81 sparse samples
 * they are visually indistinguishable.
 *
 * WARNING: Cortex-M0 has NO hardware divide and NO FPU. Every divide or modulo
 * on a non-power-of-two is a libgcc call, times 81 LEDs, times ~62 fps.
 * Everything below is shifts, masks, adds and multiplies -- keep it that way.
 * The objective check is `arm-none-eabi-nm` for __aeabi_idiv on the object.
 */
RGB_MATRIX_EFFECT(DRIFT)

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS

/* 16x16 so indexing is masks only. Authored here, never computed at runtime. */
static const uint8_t drift_noise[256] = {
     12, 194,  70, 215, 173,  19, 198, 146, 113, 186, 146,  29,  58, 149, 221,  27,
    235,   5, 115, 126, 187, 105, 221, 126,  75, 142, 202, 116, 142,  60, 109, 248,
    175, 221,  92, 149, 158,  88, 175,  15, 165, 115,  43,  58, 163,  43, 114,  29,
     53, 200,  11, 243, 156,  82,  17, 132, 131,  66, 191,  47,  22, 222,  66,  33,
    158, 231, 145, 248, 182, 152,  88,  42, 114, 152, 124,  88, 189,  85, 222,  54,
    207, 103,  62,  60, 190, 141,  35, 138, 254, 147, 250,   2, 150, 211, 153, 128,
     42,  29,  59, 188,   4, 185, 219,  26, 132,  20, 169, 185, 110, 103, 132,  98,
    146, 115, 128,  85,  16,  53,  74,  65, 192, 176, 188,  41,  90, 163,   7,  58,
    213, 168,  14,  61,  18, 166, 158,  23, 225,  80, 251, 148, 252,  45, 169,  31,
     71, 191,  92, 229, 174,  18,  97,  30, 208, 244,   8, 250, 213,  95,  68,  44,
     80, 190,  30,  51, 157,  61,  26, 236, 121,  32,  70,  59,  96, 205,  56, 129,
    197, 235, 138, 116, 170, 188,   8, 145, 139,  69, 142, 144, 140, 174,  24, 205,
    129,  55,  38, 252, 173,  95, 175, 211,  38,  14, 162, 239, 237,  68,  83, 206,
    218,  11,  19, 210,  52,  62, 211, 180,  50, 201, 115,  40, 135, 220,   2,  88,
    140, 137, 159,  30, 237,  11, 245, 199, 244,  46,  43,   6, 180, 126, 190, 212,
    228, 148,  59, 186, 213, 195, 219, 211, 152, 199, 167,  14,  30, 220, 227,   4,
};

/* The ramp is authored in HSV, not RGB, SPECIFICALLY so hue and saturation can
 * act on it: rotating an RGB ramp would need an RGB->HSV conversion per stop
 * (which QMK does not provide), while in HSV a rotation is an add.
 *
 * Dark indigo -> violet -> slate -> warm grey, weighted dark on purpose:
 * understated, not demo-pop. Each stop's v carries that dark weighting;
 * rgb_matrix_config.hsv.v scales the whole thing on top, per LED.
 *
 * NINE entries with the last duplicated so the top ramp cell needs no clamp
 * branch. */
static const hsv_t drift_ramp_hsv[9] = {
    {170, 200,  12}, {171, 205,  34}, {174, 195,  62}, {178, 175,  92},
    {182, 145, 116}, {186, 100, 126}, {192,  55, 130}, { 28,  18, 142},
    { 28,  18, 142},
};

/* Derived palette, rebuilt only when hue or saturation actually changes -- the
 * demoscene trick: keep the field, swap the palette. The per-LED path never
 * does an HSV conversion. */
static rgb_t  drift_ramp[9];
static uint8_t drift_pal_h = 0, drift_pal_s = 0;
static bool    drift_pal_valid = false;

static void drift_build_palette(uint8_t h, uint8_t s) {
    for (uint8_t i = 0; i < 9; i++) {
        hsv_t c = drift_ramp_hsv[i];
        /* Hue ADDS, so the palette keeps its internal hue relationships and
         * rotates as a whole -- the stops stay a family instead of collapsing
         * to one colour. Saturation SCALES, so turning it down washes the
         * whole ramp toward grey rather than shifting it. */
        c.h = (uint8_t)(c.h + h);
        c.s = scale8(c.s, s);
        drift_ramp[i] = rgb_matrix_hsv_to_rgb(c);
    }
    drift_pal_h = h; drift_pal_s = s; drift_pal_valid = true;
}

/* lerp with a 4-bit fraction. int16_t so (b - a) may go negative. */
static inline uint8_t drift_lerp4(uint8_t a, uint8_t b, uint8_t f) {
    return (uint8_t)(a + (((int16_t)(b - a) * f) >> 4));
}

/* Smoothstep on a 0..15 fraction, fixed point. Two multiplies, and it removes
 * the diamond-shaped grid artifact that makes linearly interpolated value noise
 * look cheap. */
static inline uint8_t drift_ease(uint8_t f) {
    return (uint8_t)(((uint16_t)f * f * (48 - 2 * f)) >> 8);
}

static bool DRIFT(effect_params_t* params) {
    /* Sub-unit precision: a cell is 16 units, and at ~62 fps a whole-unit step
     * per frame would cross the board in under a second. Low 4 bits fractional. */
    static uint16_t acc_x = 0, acc_y = 0;

    if (params->init) { acc_x = 0; acc_y = 0; drift_pal_valid = false; }

    /* Rebuild the palette only on an actual change: 9 HSV conversions when the
     * knob moves, zero on every other frame. */
    if (!drift_pal_valid || drift_pal_h != rgb_matrix_config.hsv.h
                         || drift_pal_s != rgb_matrix_config.hsv.s) {
        drift_build_palette(rgb_matrix_config.hsv.h, rgb_matrix_config.hsv.s);
    }

    /* Advance ONCE per frame. LED_PROCESS_LIMIT splits a frame into chunks, and
     * stepping per chunk would multiply the drift rate by the chunk count. */
    if (params->iter == 0) {
        uint8_t spd = rgb_matrix_config.speed;
        /* Different rate per axis: equal rates slide the field along a visible
         * diagonal, which is the axis this effect exists to hide. */
        acc_x += 1 + (spd >> 3);
        acc_y += 1 + (spd >> 4);
    }

    /* acc wraps at 65536 -> t wraps at 4096, a multiple of the 16-cell table
     * period, so the scroll stays seamless across the wrap. */
    uint16_t tx = acc_x >> 4;
    uint16_t ty = acc_y >> 4;

    uint8_t v = rgb_matrix_config.hsv.v;

    RGB_MATRIX_USE_LIMITS(led_min, led_max);
    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();

        /* The board is 217 x 59, about 3.7:1. Sampling raw would stretch every
         * feature horizontally, which reads as a bug rather than a style.
         * Compress x rather than stretching y, so both stay in range. */
        uint16_t sx = (g_led_config.point[i].x >> 2) + tx;
        uint16_t sy =  g_led_config.point[i].y       + ty;

        uint8_t gx = (sx >> 4) & 15, fx = drift_ease(sx & 15);
        uint8_t gy = (sy >> 4) & 15, fy = drift_ease(sy & 15);
        uint8_t gx1 = (gx + 1) & 15, gy1 = (gy + 1) & 15;

        uint8_t top = drift_lerp4(drift_noise[(gy  << 4) | gx],
                                  drift_noise[(gy  << 4) | gx1], fx);
        uint8_t bot = drift_lerp4(drift_noise[(gy1 << 4) | gx],
                                  drift_noise[(gy1 << 4) | gx1], fx);
        uint8_t n   = drift_lerp4(top, bot, fy);

        /* Ramp position, 5-bit fraction between adjacent stops. */
        uint8_t  idx  = n >> 5;
        uint8_t  frac = n & 31;
        const rgb_t *a = &drift_ramp[idx], *b = &drift_ramp[idx + 1];

        uint8_t r  = (uint8_t)(a->r + (((int16_t)(b->r - a->r) * frac) >> 5));
        uint8_t g  = (uint8_t)(a->g + (((int16_t)(b->g - a->g) * frac) >> 5));
        uint8_t bl = (uint8_t)(a->b + (((int16_t)(b->b - a->b) * frac) >> 5));

        /* Scale by the configured value, or the brightness keys stop working on
         * this mode. Kept as the last step so a decay trail could modulate it
         * later without touching the colour path. */
        rgb_matrix_set_color(i, scale8(r, v), scale8(g, v), scale8(bl, v));
    }
    return rgb_matrix_check_finished_leds(led_max);
}

#endif // RGB_MATRIX_CUSTOM_EFFECT_IMPLS
