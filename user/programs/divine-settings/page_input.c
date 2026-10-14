/* Sound (output, volume, system sounds) and Mouse (scrolling, double-click speed). */

#include <config.h>
#include <sound.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"

/* --- Sound ---------------------------------------------------------------------------------------- */

static struct os_audio_mixer mixer;
static bool mixer_ok;
static int volume, output;
static bool muted, effects, startup;

void page_sound_show(void) {
    mixer_ok = audio_mixer(OS_MIXER_GET, 0, &mixer) == 0;
    volume = (int)mixer.volume;
    muted = mixer.muted;
    output = mixer.output;
    effects = config_get_int(DDE_CONFIG, "SoundEffects", 1) != 0;
    startup = config_get_int(DDE_CONFIG, "StartupSound", 1) != 0;
}

void page_sound(void) {
    ui_heading("Output");
    ui_card_begin();
    if (!mixer_ok) {
        ui_info_row("No sound card was found", "");
    } else {
        const char *names[OS_AUDIO_OUTPUTS_MAX];
        for (int i = 0; i < mixer.output_count && i < OS_AUDIO_OUTPUTS_MAX; i++) {
            names[i] = mixer.outputs[i];
        }
        if (mixer.output_count > 0 && ui_choice_row("Play sound through", names, mixer.output_count, &output)) {
            audio_mixer(OS_MIXER_SET_OUTPUT, output, &mixer);
        }
        if (ui_slider_row("Output volume", &volume, 0, 100, NULL, NULL)) {
            if (mixer.muted) {
                audio_mixer(OS_MIXER_SET_MUTE, 0, &mixer);
                muted = false;
            }
            audio_mixer(OS_MIXER_SET_VOLUME, volume, &mixer);
        }
        if (ui_toggle_row("Mute", NULL, &muted)) {
            audio_mixer(OS_MIXER_SET_MUTE, muted, &mixer);
        }
    }
    ui_card_end();

    ui_heading("Sound effects");
    ui_card_begin();
    if (ui_toggle_row("Play sound effects", "Dialogs, drives plugged in or out, volume changes", &effects)) {
        config_set_int(DDE_CONFIG, "SoundEffects", effects);
    }
    if (ui_toggle_row("Play sounds at start-up and shut down", NULL, &startup)) {
        config_set_int(DDE_CONFIG, "StartupSound", startup);
    }
    if (ui_button_row("Alert sound", "What a notification sounds like", "Play", false)) {
        system_sound("Notification");
    }
    ui_card_end();
}

/* --- Mouse ---------------------------------------------------------------------------------------- */

#define SLOWEST_MS 900
#define STEP_MS    65         /* 10 steps: 900 ms ... 250 ms */

static bool natural;
static int speed;              /* 0 slow ... 10 fast */

void page_mouse_show(void) {
    natural = config_get_int(DDE_CONFIG, "NaturalScroll", 0) != 0;
    long ms = config_get_int(DDE_CONFIG, "DoubleClick", 450);
    speed = (int)((SLOWEST_MS - ms + STEP_MS / 2) / STEP_MS);
    speed = speed < 0 ? 0 : speed > 10 ? 10 : speed;
}

void page_mouse(void) {
    ui_card_begin();
    if (ui_toggle_row("Natural scrolling", "Content follows your fingers: turning the wheel down moves it up", &natural)) {
        config_set_int(DDE_CONFIG, "NaturalScroll", natural);
        gui_settings_changed(ui.g, -1, -1);
    }
    if (ui_slider_row("Double-click speed", &speed, 0, 10, "Slow", "Fast")) {
        config_set_int(DDE_CONFIG, "DoubleClick", SLOWEST_MS - speed * STEP_MS);
        gui_settings_changed(ui.g, -1, -1);
    }
    ui_card_end();
    char note[96];
    snprintf(note, sizeof(note), "Two clicks less than %d ms apart open things on the desktop.",
             SLOWEST_MS - speed * STEP_MS);
    ui_note(note);
}
