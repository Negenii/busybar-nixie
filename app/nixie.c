/**
 * @file nixie.c
 * @brief Nixie tube clock on the 72x16 front panel.
 *
 * Catalogue app: builds to a .fap, is not deployed with the firmware image.
 * The tubes themselves are in nixie_render.c, which knows nothing about the
 * device; this file is the shell around it, the same shape as fireplace.c:
 * app_shell for Start and Setup, a periodic timer driving a Canvas on the
 * front panel, a MirrorCard on the back, and three settings kept in a file.
 */

#include <furi.h>

#include <gui/gui.h>
#include <gui/modules/canvas.h>
#include <gui/modules/mirror_card.h>
#include <storage/storage.h>
#include <time/time.h>

#include "../common/app_shell.h"
#include "nixie_render.h"

#define TAG                    "Nixie"
#define NIXIE_INPUT_QUEUE_SIZE (16U)
/* Thirty frames a second: the gas is never still, and the strike is 60 ms. */
#define NIXIE_FRAME_MS (33U)
/* How often to re-read the local time in whole seconds, in case the service
 * moved it (NTP, a timezone change). The millisecond within the second comes
 * from the RTC every frame regardless. */
#define NIXIE_RESYNC_MS (10000U)

#define NIXIE_SETTINGS_DIR  "/ext/apps_data/nixie"
#define NIXIE_SETTINGS_PATH NIXIE_SETTINGS_DIR "/settings"
#define NIXIE_SETTINGS_MAGIC (0x4E35U) /* "N5": seconds, colour, depth 1..64, glow */
#define NIXIE_DEPTH_DEFAULT  (16U)
/* How long the depth gauge stays after the last click, and how long it fades. */
#define NIXIE_GAUGE_HOLD_MS (1500U)
#define NIXIE_GAUGE_FADE_MS (400U)

typedef enum {
    NixieCustomEventBack = 1UL << 0,
    NixieCustomEventStart = 1UL << 1,
    NixieCustomEventSetup = 1UL << 2,
    NixieCustomEventTune = 1UL << 3,
} NixieCustomEvent;

typedef enum {
    NixieScreenMenu,
    NixieScreenSetup,
    NixieScreenRun,
} NixieScreen;

/* Setup carries one row. Colour and depth live on the wheel while the clock
 * shows, so they are kept beside the row rather than as rows themselves. */
typedef enum {
    NixieSettingSeconds,
    NixieSettingCount,
} NixieSettingId;

static const char* nixie_seconds_labels[] = {"Off", "On"};

typedef struct {
    FuriEventLoop* event_loop;
    FuriEventLoopTimer* timer;
    FuriMessageQueue* input_queue;
    Gui* gui;
    Time* time;

    Canvas* canvas;
    MirrorCard* mirror;
    AppShellChrome chrome;
    Widget* front_widget;
    Widget* back_widget;

    NixieScreen screen;
    AppShellStart start;
    AppShellSetup setup;
    AppShellSetting setting_items[NixieSettingCount];
    AppShellSettings settings;

    NixiePreset preset;
    uint8_t* frame; /* NIXIE_FRAME_BYTES */

    /* On the wheel while the clock shows: a tap steps the colour, a turn sets
     * how alive the gas is, 1..16. */
    uint8_t colour;
    uint8_t depth;
    /* A long press of the wheel steps through the glow profiles. */
    uint8_t glow;
    /* The depth gauge on the bottom row shows until this tick, then fades. */
    uint32_t gauge_until;

    /* The clock, anchored: local seconds of the day as of the last resync,
     * and the RTC millisecond timestamp that second began at. */
    int64_t anchor_day_s;
    time_t anchor_ms;
    uint32_t last_resync_tick;
} Nixie;

static uint8_t nixie_setting(Nixie* instance, NixieSettingId id) {
    return instance->setting_items[id].value;
}

/* ---------------------------------------------------------------- settings */

static void nixie_apply_preset(Nixie* instance) {
    nixie_preset_make(&instance->preset, (NixieColour)instance->colour);
    nixie_preset_set_depth(&instance->preset, instance->depth);
    nixie_preset_set_glow(&instance->preset, (NixieGlow)instance->glow);
}

static const char* const nixie_glow_names[NixieGlowCount] = {
    "CLASSIC", "SOFT", "WARM", "SOFT WARM", "TIGHT"};

/* "NIXIE x12": the depth on the back panel's breadcrumb while the clock shows. */
static void nixie_show_depth(Nixie* instance) {
    if(!instance->mirror) return;
    AppShellText text;
    app_shell_text_reset(&text);
    app_shell_text_add(&text, nixie_glow_names[instance->glow]);
    app_shell_text_add(&text, " x");
    app_shell_text_add_num(&text, instance->depth);
    with_gui(instance->gui, { mirror_card_set_header_text(instance->mirror, text.text); });
}

/* A magic, one byte per row, then colour, depth and glow. Anything else is the defaults. */
static void nixie_settings_load(Nixie* instance) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    uint8_t buf[5 + NixieSettingCount];

    if(storage_file_open(file, NIXIE_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const size_t got = storage_file_read(file, buf, sizeof(buf));
        if((got == sizeof(buf)) && (buf[0] == (NIXIE_SETTINGS_MAGIC & 0xFFU)) &&
           (buf[1] == (NIXIE_SETTINGS_MAGIC >> 8))) {
            for(uint8_t i = 0U; i < NixieSettingCount; i++) {
                if(buf[2 + i] < instance->setting_items[i].count) {
                    instance->setting_items[i].value = buf[2 + i];
                }
            }
            const uint8_t colour = buf[2 + NixieSettingCount];
            const uint8_t depth = buf[3 + NixieSettingCount];
            const uint8_t glow = buf[4 + NixieSettingCount];
            if(colour < NixieColourCount) instance->colour = colour;
            if((depth >= NIXIE_DEPTH_MIN) && (depth <= NIXIE_DEPTH_MAX)) instance->depth = depth;
            if(glow < NixieGlowCount) instance->glow = glow;
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void nixie_settings_save(Nixie* instance) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    uint8_t buf[5 + NixieSettingCount];

    buf[0] = NIXIE_SETTINGS_MAGIC & 0xFFU;
    buf[1] = NIXIE_SETTINGS_MAGIC >> 8;
    for(uint8_t i = 0U; i < NixieSettingCount; i++) {
        buf[2 + i] = instance->setting_items[i].value;
    }
    buf[2 + NixieSettingCount] = instance->colour;
    buf[3 + NixieSettingCount] = instance->depth;
    buf[4 + NixieSettingCount] = instance->glow;

    storage_common_mkdir(storage, NIXIE_SETTINGS_DIR);
    if(storage_file_open(file, NIXIE_SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, buf, sizeof(buf));
        storage_file_close(file);
    } else {
        FURI_LOG_W(TAG, "Could not save settings");
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* -------------------------------------------------------------------- time */

static void nixie_resync(Nixie* instance) {
    /* Both reads are of the same clock, so the millisecond phase of the RTC
     * stamp is the phase of the local second: timezones are whole minutes. */
    const time_t now_ms = time_get_timestamp_ms();
    const LocalTime local = time_get_local_time(instance->time);
    const int64_t day_s =
        (int64_t)local.dt.hour * 3600 + (int64_t)local.dt.minute * 60 + (int64_t)local.dt.second;
    instance->anchor_day_s = day_s;
    instance->anchor_ms = now_ms - (now_ms % 1000);
    instance->last_resync_tick = furi_get_tick();
}

static void nixie_now(Nixie* instance, int64_t* day_s, uint32_t* ms) {
    if((furi_get_tick() - instance->last_resync_tick) >= NIXIE_RESYNC_MS) {
        nixie_resync(instance);
    }
    const time_t now_ms = time_get_timestamp_ms();
    int64_t elapsed = (int64_t)now_ms - (int64_t)instance->anchor_ms;
    if(elapsed < 0) elapsed = 0;
    *day_s = instance->anchor_day_s + elapsed / 1000;
    *ms = (uint32_t)(elapsed % 1000);
}

/* ------------------------------------------------------------------- gauge */

/* A one-pixel line along the bottom row while the wheel is being turned: a
 * faint track the whole width, filled from the left in proportion to the
 * depth, so x1 is a stub and x16 reaches the far edge. It holds for a moment
 * after the last click and fades out, over whatever the tubes drew there. */
static void nixie_draw_gauge(Nixie* instance) {
    const uint32_t now = furi_get_tick();
    if((int32_t)(instance->gauge_until - now) <= 0) return;

    const uint32_t left = instance->gauge_until - now;
    const uint32_t k = (left < NIXIE_GAUGE_FADE_MS) ? (left * 256U / NIXIE_GAUGE_FADE_MS) : 256U;
    const int32_t filled =
        ((int32_t)instance->depth * NIXIE_W + (int32_t)NIXIE_DEPTH_MAX / 2) / (int32_t)NIXIE_DEPTH_MAX;

    uint8_t* row = instance->frame + (NIXIE_H - 1) * NIXIE_W * 3;
    for(int32_t x = 0; x < NIXIE_W; x++) {
        /* The fill is the digit's own colour at two thirds; the track a tenth. */
        const uint32_t level = (x < filled) ? 170U : 26U;
        for(int n = 0; n < 3; n++) {
            row[x * 3 + n] = (uint8_t)(((uint32_t)instance->preset.core_top[n] * level * k) >> 16);
        }
    }
}

/* ------------------------------------------------------------------ canvas */

/* A canvas is drawn over whatever the front window holds and an empty one is
 * drawn as black, so it exists only while the clock does. */
static void nixie_acquire_panels(Nixie* instance) {
    if(instance->canvas) return;

    with_gui(instance->gui, {
        instance->canvas = canvas_alloc(instance->front_widget, NIXIE_W, NIXIE_H);

        instance->mirror = mirror_card_alloc(instance->back_widget);
        mirror_card_set_header_text(instance->mirror, "NIXIE");
        mirror_card_set_show_footer(instance->mirror, false);
        widget_set_align(mirror_card_get_base(instance->mirror), AlignCenter);
        widget_set_margin(mirror_card_get_base(instance->mirror), 0, 0, 2, 2);
    });
}

static void nixie_release_panels(Nixie* instance) {
    if(!instance->canvas) return;

    with_gui(instance->gui, {
        mirror_card_free(instance->mirror);
        instance->mirror = NULL;
        canvas_free(instance->canvas);
        instance->canvas = NULL;
    });
}

static void nixie_draw(Nixie* instance) {
    if(!instance->canvas) return;

    int64_t day_s;
    uint32_t ms;
    nixie_now(instance, &day_s, &ms);
    nixie_render(
        instance->frame,
        day_s,
        ms,
        &instance->preset,
        nixie_setting(instance, NixieSettingSeconds) != 0U);
    nixie_draw_gauge(instance);

    gui_lock(instance->gui);
    canvas_draw_begin(instance->canvas);
    canvas_clear(instance->canvas);

    const uint8_t* p = instance->frame;
    for(int32_t y = 0; y < NIXIE_H; y++) {
        for(int32_t x = 0; x < NIXIE_W; x++, p += 3) {
            if((p[0] | p[1] | p[2]) == 0U) continue;
            canvas_draw_pixel(
                instance->canvas, x, y, (Color){.r = p[0], .g = p[1], .b = p[2], .a = 255});
        }
    }

    canvas_draw_end(instance->canvas);
    gui_unlock(instance->gui);
}

/* ----------------------------------------------------------------- screens */

static void nixie_dialog_callback(uint8_t index, void* context);
static void nixie_setup_changed(void* context, uint8_t index);

static void nixie_show_menu(Nixie* instance) {
    instance->screen = NixieScreenMenu;

    nixie_release_panels(instance);
    with_gui(instance->gui, {
        app_shell_chrome_leave_setup(&instance->chrome);
        app_shell_setup_hide(&instance->setup);
        app_shell_start_show(
            &instance->start,
            instance->front_widget,
            instance->back_widget,
            "Nixie",
            APP_SHELL_ICON("Nixie"),
            nixie_dialog_callback,
            instance);
    });
}

static void nixie_show_setup(Nixie* instance) {
    instance->screen = NixieScreenSetup;

    with_gui(instance->gui, {
        app_shell_chrome_enter_setup(&instance->chrome);
        app_shell_start_hide(&instance->start);
        app_shell_setup_show(
            &instance->setup,
            instance->front_widget,
            instance->back_widget,
            &instance->settings,
            nixie_setup_changed,
            instance);
    });
}

static void nixie_enter_run(Nixie* instance) {
    instance->screen = NixieScreenRun;

    with_gui(instance->gui, {
        app_shell_chrome_leave_setup(&instance->chrome);
        app_shell_start_hide(&instance->start);
        app_shell_setup_hide(&instance->setup);
    });

    nixie_apply_preset(instance);
    nixie_resync(instance);
    nixie_acquire_panels(instance);
    nixie_show_depth(instance);
    nixie_draw(instance);
    furi_event_loop_timer_start(instance->timer, NIXIE_FRAME_MS);
}

static void nixie_leave_run(Nixie* instance) {
    furi_event_loop_timer_stop(instance->timer);
    nixie_show_menu(instance);
}

/* Posts rather than acts: the Dialog calls this from inside input dispatch,
 * which already holds the GUI lock. */
static void nixie_dialog_callback(uint8_t index, void* context) {
    Nixie* instance = context;

    furi_event_loop_set_custom_event(
        instance->event_loop,
        (index == AppShellChoiceStart) ? NixieCustomEventStart : NixieCustomEventSetup);
}

static void nixie_setup_changed(void* context, uint8_t index) {
    UNUSED(index);
    Nixie* instance = context;
    nixie_settings_save(instance);
}

static void nixie_timer_callback(void* context) {
    Nixie* instance = context;
    nixie_draw(instance);
}

static void nixie_handle_key(Nixie* instance, InputKey key) {
    switch(instance->screen) {
    case NixieScreenMenu:
        if(key == InputKeyBack) {
            furi_event_loop_stop(instance->event_loop);
        }
        return;

    case NixieScreenSetup:
        if(key == InputKeyBack) {
            nixie_show_menu(instance);
        }
        return;

    case NixieScreenRun:
        if(key == InputKeyBack) {
            nixie_leave_run(instance);
            return;
        }
        /* A tap of the wheel steps the colour without leaving the clock. */
        if(key == InputKeyOk) {
            instance->colour = (uint8_t)((instance->colour + 1U) % NixieColourCount);
            nixie_apply_preset(instance);
            nixie_settings_save(instance);
            nixie_draw(instance);
        } else if((key == InputKeyUp) || (key == InputKeyDown)) {
            /* The wheel deepens or calms the gas without leaving the clock. */
            if((key == InputKeyUp) && (instance->depth < NIXIE_DEPTH_MAX)) instance->depth++;
            if((key == InputKeyDown) && (instance->depth > NIXIE_DEPTH_MIN)) instance->depth--;
            nixie_apply_preset(instance);
            nixie_show_depth(instance);
            instance->gauge_until = furi_get_tick() + NIXIE_GAUGE_HOLD_MS;
            nixie_draw(instance);
            nixie_settings_save(instance);
        }
        return;
    }
}

/* ---------------------------------------------------------------- plumbing */

static bool nixie_input_callback(const InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);
    Nixie* instance = context;

    bool consumed = false;

    if((event->type == InputTypeLong) && (event->key == InputKeyOk)) {
        furi_event_loop_set_custom_event(instance->event_loop, NixieCustomEventTune);
        return true;
    }

    if(event->type == InputTypeShort) {
        if(event->key == InputKeyBack) {
            furi_event_loop_set_custom_event(instance->event_loop, NixieCustomEventBack);
            consumed = true;
        } else if(
            (event->key == InputKeyUp) || (event->key == InputKeyDown) ||
            (event->key == InputKeyOk) || (event->key == InputKeyStart)) {
            if(furi_message_queue_put(instance->input_queue, &event->key, 0U) != FuriStatusOk) {
                FURI_LOG_W(TAG, "Input queue full; dropping %s", input_get_key_name(event->key));
            }
            consumed = true;
        }
    }

    return consumed;
}

static void nixie_input_queue_callback(FuriEventLoopObject* object, void* context) {
    furi_assert(context);
    Nixie* instance = context;
    furi_assert(object == instance->input_queue);

    InputKey key;
    while(furi_message_queue_get(instance->input_queue, &key, 0U) == FuriStatusOk) {
        nixie_handle_key(instance, key);
    }
}

static void nixie_custom_event_callback(uint32_t events, void* context) {
    furi_assert(context);
    Nixie* instance = context;

    if(events & NixieCustomEventBack) {
        nixie_handle_key(instance, InputKeyBack);
    }
    if(events & NixieCustomEventStart) {
        nixie_enter_run(instance);
    }
    if(events & NixieCustomEventSetup) {
        nixie_show_setup(instance);
    }
    if((events & NixieCustomEventTune) && (instance->screen == NixieScreenRun)) {
        instance->glow = (uint8_t)((instance->glow + 1U) % NixieGlowCount);
        nixie_apply_preset(instance);
        nixie_show_depth(instance);
        nixie_draw(instance);
        nixie_settings_save(instance);
    }
}

static bool nixie_signal_callback(uint32_t signal, void* arg, void* context) {
    UNUSED(arg);
    Nixie* instance = context;

    switch(signal) {
    case FuriSignalExit:
        furi_event_loop_stop(instance->event_loop);
        return true;

    default:
        return false;
    }
}

static void nixie_settings_init(Nixie* instance) {
    instance->setting_items[NixieSettingSeconds] =
        (AppShellSetting){"Seconds", nixie_seconds_labels, COUNT_OF(nixie_seconds_labels), 0U};

    instance->settings.items = instance->setting_items;
    instance->settings.count = NixieSettingCount;
    instance->settings.index = 0U;
    instance->settings.top = 0U;
}

static Nixie* nixie_alloc(void) {
    Nixie* instance = malloc(sizeof(Nixie));
    memset(instance, 0, sizeof(Nixie));

    instance->frame = malloc(NIXIE_FRAME_BYTES);

    instance->event_loop = furi_event_loop_alloc();
    instance->input_queue = furi_message_queue_alloc(NIXIE_INPUT_QUEUE_SIZE, sizeof(InputKey));
    instance->gui = furi_record_open(RECORD_GUI);
    instance->time = furi_record_open(RECORD_TIME);

    instance->screen = NixieScreenMenu;

    instance->colour = NixieColourOrange;
    instance->depth = NIXIE_DEPTH_DEFAULT;
    instance->glow = NixieGlowClassic;
    nixie_settings_init(instance);
    nixie_settings_load(instance);
    nixie_apply_preset(instance);

    instance->timer = furi_event_loop_timer_alloc(
        instance->event_loop, nixie_timer_callback, FuriEventLoopTimerTypePeriodic, instance);

    furi_event_loop_subscribe_message_queue(
        instance->event_loop,
        instance->input_queue,
        FuriEventLoopEventIn,
        nixie_input_queue_callback,
        instance);

    furi_thread_set_signal_callback(furi_thread_get_current(), nixie_signal_callback, instance);

    furi_event_loop_set_custom_event_callback(
        instance->event_loop, nixie_custom_event_callback, instance);

    with_gui(instance->gui, {
        GuiLayer* main_layer = gui_get_layer(instance->gui, GuiLayerIdMain);
        gui_layer_add_input_callback(main_layer, nixie_input_callback, instance);

        app_shell_chrome_init(
            &instance->chrome,
            gui_layer_get_root_widget(main_layer, GuiDisplayIdFront),
            gui_layer_get_root_widget(main_layer, GuiDisplayIdBack),
            "NIXIE");
        instance->front_widget = instance->chrome.front_window;
        instance->back_widget = instance->chrome.back_window;
    });

    nixie_show_menu(instance);

    return instance;
}

static void nixie_free(Nixie* instance) {
    furi_thread_set_signal_callback(furi_thread_get_current(), NULL, NULL);

    furi_event_loop_timer_stop(instance->timer);
    nixie_release_panels(instance);

    with_gui(instance->gui, {
        GuiLayer* main_layer = gui_get_layer(instance->gui, GuiLayerIdMain);
        gui_layer_remove_input_callback(main_layer, nixie_input_callback);

        app_shell_start_hide(&instance->start);
        app_shell_setup_hide(&instance->setup);
        app_shell_chrome_free(&instance->chrome);
    });

    furi_record_close(RECORD_TIME);
    furi_record_close(RECORD_GUI);

    furi_event_loop_unsubscribe(instance->event_loop, instance->input_queue);
    furi_message_queue_free(instance->input_queue);
    furi_event_loop_timer_free(instance->timer);
    furi_event_loop_free(instance->event_loop);

    free(instance->frame);
    free(instance);
}

int32_t nixie_app(void* arg) {
    UNUSED(arg);

    FURI_LOG_I(TAG, "Nixie started");

    Nixie* instance = nixie_alloc();
    furi_event_loop_run(instance->event_loop);
    nixie_free(instance);

    FURI_LOG_I(TAG, "Nixie exiting");

    return 0;
}
