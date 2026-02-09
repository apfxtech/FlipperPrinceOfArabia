#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wredundant-decls"

#include "lib/Arduboy2.h"
#include "lib/ArduboyFX.h"
#include "src/utils/Arduboy2Ext.h"
#include "src/ArduboyTonesFX.h"
#include "src/entities/Entities.h"
#include "src/utils/Enums.h"

#define DISPLAY_WIDTH  128
#define DISPLAY_HEIGHT 64
#define FB_SIZE        (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

FuriMessageQueue* g_arduboy_sound_queue = NULL;
FuriThread* g_arduboy_sound_thread = NULL;
volatile bool g_arduboy_sound_thread_running = false;
volatile bool g_arduboy_audio_enabled = false;
volatile bool g_arduboy_tones_playing = false;
volatile bool g_arduboy_force_high = false;
volatile bool g_arduboy_force_norm = false;
volatile uint8_t g_arduboy_volume_mode = VOLUME_IN_TONE;

void splashScreen_Init();
void splashScreen();
void title_Init();
void saveSoundState();
void title();
void setSound(SoundIndex index);
void isEnemyVisible(
    Prince& prince,
    bool swapEnemies,
    bool& isVisible,
    bool& sameLevelAsPrince,
    bool justEnteredRoom);

bool testScroll(GamePlay& gamePlay, Prince& prince, Level& level);
void pushSequence();
void processJump(uint24_t pos);
void processRunJump(Prince& prince, Level& level, bool testEnemy);
void processStandingJump(Prince& prince, Level& level);
void initFlash(Prince& prince, Level& level, FlashType flashType);
void initFlash(Enemy& enemy, Level& level, FlashType flashType);
uint8_t activateSpikes(Prince& prince, Level& level);
void activateSpikes_Helper(Item& spikes);
void pushJumpUp_Drop(Prince& prince);
bool leaveLevel(Prince& prince, Level& level);
void pushDead(Prince& entity, Level& level, GamePlay& gamePlay, bool clear, DeathType deathType);
void pushDead(Enemy& entity, bool clear);
void showSign(Prince& prince, Level& level);
void playGrab();
void fixPosition();
uint8_t getImageIndexFromStance(uint16_t stance);
void getStance_Offsets(Direction direction, Point& offset, int16_t stance);
void processRunningTurn();
void saveCookie(bool enableLEDs);
void handleBlades();
void handleBlade_Single(int8_t tileXIdx, int8_t tileYIdx, uint8_t princeLX, uint8_t princeRX);

void game_Init();
void game_StartLevel();
uint16_t getMenuData(uint24_t table);
void game();
void moveBackwardsWithSword(Prince& prince);
void moveBackwardsWithSword(BaseEntity entity, BaseStack stack);

void render(bool sameLevelAsPrince);
void renderMenu();
void renderNumber(uint8_t x, uint8_t y, uint8_t number);
void renderNumber_Small(uint8_t x, uint8_t y, uint8_t number);
void renderNumber_Upright(uint8_t x, uint8_t y, uint8_t number);
void renderTorches(uint8_t x1, uint8_t x2, uint8_t y);

void setup();
void loop();

#include "game/PrinceOfArabia.cpp"
#include "game/PrinceOfArabia_Game.cpp"
#include "game/PrinceOfArabia_Render.cpp"
#include "game/PrinceOfArabia_SplashScreen.cpp"
#include "game/PrinceOfArabia_Title.cpp"
#include "game/PrinceOfArabia_Utils.cpp"

typedef struct {
    uint8_t screen_buffer[FB_SIZE];
    uint8_t front_buffer[FB_SIZE];

    Gui* gui;
    Canvas* canvas;
    FuriMutex* fb_mutex;
    FuriMutex* game_mutex;

    FuriPubSub* input_events;
    FuriPubSubSubscription* input_sub;

    volatile uint8_t input_state;
    volatile bool exit_requested;
} FlipperState;

typedef struct {
    volatile bool* exit_requested;
    Arduboy2Base::InputContext* input_ctx;
} InputBridge;

static FlipperState* g_state = NULL;
static InputBridge g_input_bridge = {};

static void input_events_callback(const void* value, void* ctx) {
    if(!value || !ctx) return;

    const InputEvent* event = (const InputEvent*)value;
    InputBridge* bridge = (InputBridge*)ctx;

    if(event->key == InputKeyBack && event->type == InputTypeLong) {
        if(bridge->exit_requested) {
            *bridge->exit_requested = true;
        }
        return;
    }

    InputEvent tmp = *event;
    Arduboy2Base::FlipperInputCallback(&tmp, bridge->input_ctx);
}

static void framebuffer_commit_callback(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context) {
    UNUSED(orientation);

    FlipperState* state = (FlipperState*)context;
    if(!state || !data || size < FB_SIZE) return;

    if(furi_mutex_acquire(state->fb_mutex, 0) != FuriStatusOk) return;
    const uint8_t* src = state->front_buffer;
    for(size_t i = 0; i < FB_SIZE; i++) {
        data[i] = (uint8_t)(src[i] ^ 0xFF);
    }
    furi_mutex_release(state->fb_mutex);
}

extern "C" int32_t arduboy_app(void* p) {
    UNUSED(p);

    g_state = (FlipperState*)malloc(sizeof(FlipperState));
    if(!g_state) return -1;
    memset(g_state, 0, sizeof(FlipperState));

    g_state->game_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    g_state->fb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!g_state->game_mutex || !g_state->fb_mutex) {
        if(g_state->game_mutex) furi_mutex_free(g_state->game_mutex);
        if(g_state->fb_mutex) furi_mutex_free(g_state->fb_mutex);
        free(g_state);
        g_state = NULL;
        return -1;
    }

    g_state->exit_requested = false;
    g_state->input_state = 0;
    memset(g_state->screen_buffer, 0x00, FB_SIZE);
    memset(g_state->front_buffer, 0x00, FB_SIZE);

    arduboy.begin(
        g_state->screen_buffer,
        &g_state->input_state,
        g_state->game_mutex,
        &g_state->exit_requested);
    Sprites::setArduboy(&arduboy);

    EEPROM.begin(APP_DATA_PATH("eeprom.bin"));

    g_state->gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_framebuffer_callback(g_state->gui, framebuffer_commit_callback, g_state);
    g_state->canvas = gui_direct_draw_acquire(g_state->gui);

    g_state->input_events = (FuriPubSub*)furi_record_open(RECORD_INPUT_EVENTS);
    g_input_bridge.exit_requested = &g_state->exit_requested;
    g_input_bridge.input_ctx = arduboy.inputContext();
    g_state->input_sub =
        furi_pubsub_subscribe(g_state->input_events, input_events_callback, &g_input_bridge);

    if(furi_mutex_acquire(g_state->game_mutex, FuriWaitForever) == FuriStatusOk) {
        const uint32_t frame_before = arduboy.frameCount();
        setup();
        loop();
        const uint32_t frame_after = arduboy.frameCount();
        if(frame_after != frame_before) {
            if(furi_mutex_acquire(g_state->fb_mutex, FuriWaitForever) == FuriStatusOk) {
                memcpy(g_state->front_buffer, g_state->screen_buffer, FB_SIZE);
                furi_mutex_release(g_state->fb_mutex);
            }
            arduboy.applyDeferredDisplayOps();
        }
        furi_mutex_release(g_state->game_mutex);
    }
    if(g_state->canvas) canvas_commit(g_state->canvas);

    while(!g_state->exit_requested) {
        if(furi_mutex_acquire(g_state->game_mutex, 0) == FuriStatusOk) {
            const uint32_t frame_before = arduboy.frameCount();
            loop();
            const uint32_t frame_after = arduboy.frameCount();
            if(frame_after != frame_before) {
                if(furi_mutex_acquire(g_state->fb_mutex, 0) == FuriStatusOk) {
                    memcpy(g_state->front_buffer, g_state->screen_buffer, FB_SIZE);
                    furi_mutex_release(g_state->fb_mutex);
                }
                arduboy.applyDeferredDisplayOps();
            }
            furi_mutex_release(g_state->game_mutex);
        }
        if(g_state->canvas) canvas_commit(g_state->canvas);
        furi_delay_ms(1);
    }

    arduboy.audio.saveOnOff();
    EEPROM.commit();
    FX::commit();
    FX::end();
    arduboy_tone_sound_system_deinit();

    if(g_state->input_sub) {
        furi_pubsub_unsubscribe(g_state->input_events, g_state->input_sub);
        g_state->input_sub = NULL;
    }

    if(g_state->input_events) {
        furi_record_close(RECORD_INPUT_EVENTS);
        g_state->input_events = NULL;
    }

    if(g_state->gui) {
        gui_direct_draw_release(g_state->gui);
        gui_remove_framebuffer_callback(g_state->gui, framebuffer_commit_callback, g_state);
        furi_record_close(RECORD_GUI);
        g_state->gui = NULL;
        g_state->canvas = NULL;
    }

    if(g_state->game_mutex) {
        furi_mutex_free(g_state->game_mutex);
        g_state->game_mutex = NULL;
    }
    if(g_state->fb_mutex) {
        furi_mutex_free(g_state->fb_mutex);
        g_state->fb_mutex = NULL;
    }

    free(g_state);
    g_state = NULL;

    return 0;
}

#pragma GCC diagnostic pop
