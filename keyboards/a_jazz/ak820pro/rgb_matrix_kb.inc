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
