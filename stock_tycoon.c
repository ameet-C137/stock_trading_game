/*
 * Stock Tycoon - a stock trading game for Flipper Zero
 * By Ameet
 *
 * Buy low, sell high, and grow your net worth before the market closes.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_speaker.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "StockTycoon"

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define HISTORY_LEN         48
#define STARTING_CASH_CENTS 1000000 /* $10,000.00 */
#define STARTING_PRICE_CENTS 10000  /* $100.00 */
#define ROUND_LIMIT          80     /* number of price ticks per game */
#define TICKS_PER_SEC         4     /* timer frequency */
#define TICKS_PER_ROUND        6    /* price updates every 1.5s */
#define MESSAGE_TICKS           6
#define POPUP_TICKS              8
#define EVENT_TICKS                10
#define HIGHSCORE_FILE APP_DATA_PATH("highscore.txt")

static const uint8_t TRADE_QTY[3] = {1, 3, 5};

/* Simple looping chiptune melody. 0 = rest. Values are Hz. */
static const uint16_t MELODY[] = {
    523, 0, 659, 0, 784, 0, 659, 0,
    523, 0, 587, 0, 698, 0, 587, 0,
    523, 0, 659, 0, 784, 880, 784, 659,
    698, 0, 587, 0, 523, 0, 0,   0,
};
#define MELODY_LEN (sizeof(MELODY) / sizeof(MELODY[0]))

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    GameStateSplash,
    GameStateMenu,
    GameStateHelp,
    GameStateSettings,
    GameStatePlaying,
    GameStateTrade,
    GameStateConfirmQuit,
    GameStateGameOver,
} GameState;

typedef enum {
    TradeBuy,
    TradeSell,
} TradeMode;

typedef enum {
    EventTypeKey,
    EventTypeTick,
} AppEventType;

typedef struct {
    AppEventType type;
    InputEvent input;
} AppEvent;

typedef struct {
    GameState state;

    /* menu */
    uint8_t menu_index;
    uint8_t help_page;

    /* settings */
    bool sound_on;
    uint8_t difficulty; /* 0 easy, 1 normal, 2 hard */
    uint8_t settings_row;

    /* market */
    int32_t price_cents;
    int32_t last_price_cents;
    int32_t price_history[HISTORY_LEN];
    uint8_t history_count;
    int32_t trend_bias;
    uint16_t trend_ticks_left;
    uint32_t round;
    uint32_t sub_tick;

    /* portfolio */
    int32_t cash_cents;
    int32_t shares;
    int32_t start_net_worth_cents;
    int32_t best_net_worth_cents;

    /* trade overlay */
    TradeMode trade_mode;
    uint8_t trade_qty_index;

    /* feedback */
    char message[40];
    uint8_t message_ticks;
    char popup_text[24];
    int8_t popup_y_offset;
    uint8_t popup_ticks;
    bool popup_positive;

    /* market events */
    bool event_active;
    char event_text[28];
    uint8_t event_ticks;
    int32_t event_bias;

    /* splash animation */
    uint16_t splash_ticks;

    /* audio */
    bool speaker_available;
    uint16_t music_step;
    int16_t sfx_ticks_remaining;
    uint16_t sfx_freq;

    /* game over */
    bool new_high_score;

    FuriMutex* mutex;
    ViewPort* view_port;
    Gui* gui;
    FuriMessageQueue* queue;
    FuriTimer* timer;
    NotificationApp* notification;
    bool running;
} StockTycoonApp;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void format_cents(int32_t cents, char* buf, size_t buf_size) {
    bool neg = cents < 0;
    if(neg) cents = -cents;
    long dollars = cents / 100;
    long rem = cents % 100;
    snprintf(buf, buf_size, "%s$%ld.%02ld", neg ? "-" : "", dollars, rem);
}

static int32_t net_worth(StockTycoonApp* app) {
    return app->cash_cents + app->shares * app->price_cents;
}

static void set_message(StockTycoonApp* app, const char* text) {
    strncpy(app->message, text, sizeof(app->message) - 1);
    app->message[sizeof(app->message) - 1] = '\0';
    app->message_ticks = MESSAGE_TICKS;
}

static void set_popup(StockTycoonApp* app, const char* text, bool positive) {
    strncpy(app->popup_text, text, sizeof(app->popup_text) - 1);
    app->popup_text[sizeof(app->popup_text) - 1] = '\0';
    app->popup_ticks = POPUP_TICKS;
    app->popup_y_offset = 0;
    app->popup_positive = positive;
}

static void play_sfx(StockTycoonApp* app, uint16_t freq, uint16_t ticks) {
    if(!app->sound_on || !app->speaker_available) return;
    app->sfx_freq = freq;
    app->sfx_ticks_remaining = ticks;
}

static void notify_ok(StockTycoonApp* app) {
    if(app->notification) notification_message(app->notification, &sequence_success);
}

static void notify_bad(StockTycoonApp* app) {
    if(app->notification) notification_message(app->notification, &sequence_error);
}

/* ------------------------------------------------------------------ */
/* High score persistence                                              */
/* ------------------------------------------------------------------ */

static int32_t load_high_score(void) {
    int32_t score = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, HIGHSCORE_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[16] = {0};
        uint16_t read = storage_file_read(file, buf, sizeof(buf) - 1);
        if(read > 0) {
            buf[read] = '\0';
            score = (int32_t)strtol(buf, NULL, 10);
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return score;
}

static void save_high_score(int32_t score) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, HIGHSCORE_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%ld", (long)score);
        storage_file_write(file, buf, len);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* ------------------------------------------------------------------ */
/* Market simulation                                                   */
/* ------------------------------------------------------------------ */

static int32_t rand_range(int32_t lo, int32_t hi) {
    if(hi <= lo) return lo;
    uint32_t r = furi_hal_random_get();
    return lo + (int32_t)(r % (uint32_t)(hi - lo + 1));
}

static void push_history(StockTycoonApp* app, int32_t price) {
    if(app->history_count < HISTORY_LEN) {
        app->price_history[app->history_count] = price;
        app->history_count++;
    } else {
        memmove(
            &app->price_history[0],
            &app->price_history[1],
            sizeof(int32_t) * (HISTORY_LEN - 1));
        app->price_history[HISTORY_LEN - 1] = price;
    }
}

static void maybe_trigger_event(StockTycoonApp* app) {
    if(app->event_active) return;
    /* ~6% chance per round of a market event */
    int32_t roll = rand_range(0, 99);
    if(roll < 94) return;

    int32_t kind = rand_range(0, 3);
    switch(kind) {
    case 0:
        strncpy(app->event_text, "MARKET CRASH!", sizeof(app->event_text) - 1);
        app->event_bias = -(app->price_cents * 22) / 100;
        break;
    case 1:
        strncpy(app->event_text, "BULL RUN!", sizeof(app->event_text) - 1);
        app->event_bias = (app->price_cents * 22) / 100;
        break;
    case 2:
        strncpy(app->event_text, "BREAKING NEWS!", sizeof(app->event_text) - 1);
        app->event_bias = (app->price_cents * 10) / 100;
        break;
    default:
        strncpy(app->event_text, "SUPPLY SHOCK!", sizeof(app->event_text) - 1);
        app->event_bias = -(app->price_cents * 10) / 100;
        break;
    }
    app->event_active = true;
    app->event_ticks = EVENT_TICKS;
    notify_bad(app);
}

static void update_price(StockTycoonApp* app) {
    app->last_price_cents = app->price_cents;

    /* volatility depends on difficulty */
    int32_t base_vol;
    if(app->difficulty == 0) {
        base_vol = 180; /* cents */
    } else if(app->difficulty == 1) {
        base_vol = 320;
    } else {
        base_vol = 520;
    }

    /* occasionally change the drift/trend bias so price feels alive */
    if(app->trend_ticks_left == 0) {
        app->trend_bias = rand_range(-120, 120);
        app->trend_ticks_left = (uint16_t)rand_range(3, 8);
    } else {
        app->trend_ticks_left--;
    }

    int32_t noise = rand_range(-base_vol, base_vol);
    int32_t delta = noise + app->trend_bias;

    maybe_trigger_event(app);
    if(app->event_active) {
        delta += app->event_bias / (int32_t)EVENT_TICKS;
    }

    app->price_cents += delta;
    if(app->price_cents < 100) app->price_cents = 100; /* floor at $1.00 */
    if(app->price_cents > 999900) app->price_cents = 999900;

    push_history(app, app->price_cents);
    app->round++;
}

/* ------------------------------------------------------------------ */
/* Game control                                                        */
/* ------------------------------------------------------------------ */

static void start_new_game(StockTycoonApp* app) {
    app->cash_cents = STARTING_CASH_CENTS;
    app->shares = 0;
    app->price_cents = STARTING_PRICE_CENTS;
    app->last_price_cents = STARTING_PRICE_CENTS;
    app->history_count = 0;
    memset(app->price_history, 0, sizeof(app->price_history));
    push_history(app, app->price_cents);
    app->trend_bias = 0;
    app->trend_ticks_left = 0;
    app->round = 0;
    app->sub_tick = 0;
    app->message[0] = '\0';
    app->message_ticks = 0;
    app->popup_text[0] = '\0';
    app->popup_ticks = 0;
    app->event_active = false;
    app->event_ticks = 0;
    app->start_net_worth_cents = net_worth(app);
    app->state = GameStatePlaying;
}

static void do_trade(StockTycoonApp* app) {
    int32_t qty = TRADE_QTY[app->trade_qty_index];
    int32_t cost = qty * app->price_cents;
    char buf[24];

    if(app->trade_mode == TradeBuy) {
        if(app->cash_cents >= cost) {
            app->cash_cents -= cost;
            app->shares += qty;
            format_cents(cost, buf, sizeof(buf));
            char popup[32];
            snprintf(popup, sizeof(popup), "-%s", buf);
            set_popup(app, popup, false);
            set_message(app, "Bought!");
            play_sfx(app, 1200, 2);
            notify_ok(app);
            app->state = GameStatePlaying;
        } else {
            set_message(app, "Not enough cash!");
            play_sfx(app, 200, 3);
            notify_bad(app);
        }
    } else {
        if(app->shares >= qty) {
            app->shares -= qty;
            app->cash_cents += cost;
            format_cents(cost, buf, sizeof(buf));
            char popup[32];
            snprintf(popup, sizeof(popup), "+%s", buf);
            set_popup(app, popup, true);
            set_message(app, "Sold!");
            play_sfx(app, 900, 2);
            notify_ok(app);
            app->state = GameStatePlaying;
        } else {
            set_message(app, "Not enough shares!");
            play_sfx(app, 200, 3);
            notify_bad(app);
        }
    }
}

static void end_game(StockTycoonApp* app) {
    int32_t final_worth = net_worth(app);
    app->new_high_score = final_worth > app->best_net_worth_cents;
    if(app->new_high_score) {
        app->best_net_worth_cents = final_worth;
        save_high_score(final_worth);
    }
    app->state = GameStateGameOver;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_header_bar(Canvas* canvas, const char* title) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, title);
    canvas_draw_line(canvas, 0, 12, 127, 12);
}

static void draw_splash(Canvas* canvas, StockTycoonApp* app) {
    canvas_set_font(canvas, FontPrimary);

    /* title reveal animation */
    const char* title = "STOCK TYCOON";
    size_t total = strlen(title);
    size_t reveal = app->splash_ticks / 2;
    if(reveal > total) reveal = total;
    char buf[16];
    strncpy(buf, title, reveal);
    buf[reveal] = '\0';
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignBottom, buf);

    /* tiny animated candlestick chart under the title */
    int32_t bars[10] = {20, 30, 18, 34, 26, 40, 22, 36, 28, 42};
    for(int i = 0; i < 10; i++) {
        int32_t h = bars[(i + app->splash_ticks / 3) % 10] / 2;
        int x = 14 + i * 10;
        int y = 40;
        canvas_draw_box(canvas, x, y - h, 4, h);
    }

    if((app->splash_ticks / 4) % 2 == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignBottom, "Press OK to start");
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "ameet");
}

static void draw_menu(Canvas* canvas, StockTycoonApp* app) {
    draw_header_bar(canvas, "Stock Tycoon");
    const char* items[] = {"Play", "How To Play", "Settings", "Credits"};
    canvas_set_font(canvas, FontSecondary);
    for(int i = 0; i < 4; i++) {
        int y = 24 + i * 10;
        if((uint8_t)i == app->menu_index) {
            canvas_draw_box(canvas, 8, y - 8, 112, 10);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, 14, y, items[i]);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, 14, y, items[i]);
        }
    }
    char hs[24];
    format_cents(app->best_net_worth_cents, hs, sizeof(hs));
    char line[32];
    snprintf(line, sizeof(line), "Best: %s", hs);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, line);
}

static void draw_help(Canvas* canvas, StockTycoonApp* app) {
    canvas_set_font(canvas, FontSecondary);
    if(app->help_page == 2) {
        draw_header_bar(canvas, "Credits");
        canvas_draw_str(canvas, 2, 24, "Stock Tycoon v1.0");
        canvas_draw_str(canvas, 2, 36, "Game by: ameet");
        canvas_draw_str(canvas, 2, 48, "Built for Flipper Zero");
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK / BACK: menu");
        return;
    }

    draw_header_bar(canvas, "How To Play");
    if(app->help_page == 0) {
        canvas_draw_str(canvas, 2, 22, "LEFT = Sell menu");
        canvas_draw_str(canvas, 2, 32, "RIGHT = Buy menu");
        canvas_draw_str(canvas, 2, 42, "Pick qty: 1 / 3 / 5");
        canvas_draw_str(canvas, 2, 52, "OK = confirm trade");
    } else {
        canvas_draw_str(canvas, 2, 22, "Price moves live.");
        canvas_draw_str(canvas, 2, 32, "Watch for events like");
        canvas_draw_str(canvas, 2, 42, "CRASH! or BULL RUN!");
        canvas_draw_str(canvas, 2, 52, "Grow net worth to win.");
    }
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: next  BACK: menu");
}

static void draw_settings(Canvas* canvas, StockTycoonApp* app) {
    draw_header_bar(canvas, "Settings");
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_str(canvas, 4, 26, app->settings_row == 0 ? ">Sound:" : " Sound:");
    canvas_draw_str(canvas, 70, 26, app->sound_on ? "ON" : "OFF");
    if(!app->speaker_available) {
        canvas_draw_str(canvas, 4, 36, "(speaker busy)");
    }

    canvas_draw_str(canvas, 4, 48, app->settings_row == 1 ? ">Difficulty:" : " Difficulty:");
    const char* diffs[] = {"Easy", "Normal", "Hard"};
    canvas_draw_str(canvas, 82, 48, diffs[app->difficulty]);

    canvas_draw_str_aligned(
        canvas, 64, 63, AlignCenter, AlignBottom, "UP/DN move  OK toggle");
}

static void draw_chart(Canvas* canvas, StockTycoonApp* app, int x0, int y0, int w, int h) {
    if(app->history_count < 2) return;

    int32_t min_p = app->price_history[0];
    int32_t max_p = app->price_history[0];
    for(uint8_t i = 1; i < app->history_count; i++) {
        if(app->price_history[i] < min_p) min_p = app->price_history[i];
        if(app->price_history[i] > max_p) max_p = app->price_history[i];
    }
    if(max_p == min_p) max_p = min_p + 100;

    int32_t range = max_p - min_p;
    int prev_x = 0, prev_y = 0;
    for(uint8_t i = 0; i < app->history_count; i++) {
        int x = x0 + (int)((long)i * w / (HISTORY_LEN - 1));
        int32_t norm = ((app->price_history[i] - min_p) * (h - 2)) / range;
        int y = y0 + h - 2 - (int)norm;
        if(i > 0) {
            canvas_draw_line(canvas, prev_x, prev_y, x, y);
        }
        prev_x = x;
        prev_y = y;
    }
}

static void draw_playing(Canvas* canvas, StockTycoonApp* app) {
    char buf[24];

    /* top bar: price + trend arrow */
    canvas_set_font(canvas, FontPrimary);
    format_cents(app->price_cents, buf, sizeof(buf));
    canvas_draw_str(canvas, 2, 9, buf);

    const char* arrow = "-";
    if(app->price_cents > app->last_price_cents)
        arrow = "^";
    else if(app->price_cents < app->last_price_cents)
        arrow = "v";
    canvas_draw_str(canvas, 78, 9, arrow);

    canvas_set_font(canvas, FontSecondary);
    if(app->message_ticks > 0) {
        canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, app->message);
    } else {
        char round_buf[16];
        snprintf(
            round_buf,
            sizeof(round_buf),
            "Day %lu/%lu",
            (unsigned long)app->round,
            (unsigned long)ROUND_LIMIT);
        canvas_draw_str_aligned(canvas, 126, 9, AlignRight, AlignBottom, round_buf);
    }

    canvas_draw_line(canvas, 0, 12, 127, 12);

    /* chart area */
    draw_chart(canvas, app, 2, 14, 124, 28);

    if(app->event_active) {
        canvas_draw_box(canvas, 14, 16, 100, 10);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignBottom, app->event_text);
        canvas_set_color(canvas, ColorBlack);
    }

    /* popup (+/- cash floating text) */
    if(app->popup_ticks > 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 100, 30 + app->popup_y_offset, AlignCenter, AlignBottom, app->popup_text);
    }

    /* bottom stats bar */
    canvas_draw_line(canvas, 0, 44, 127, 44);
    canvas_set_font(canvas, FontSecondary);

    format_cents(app->cash_cents, buf, sizeof(buf));
    char cash_line[32];
    snprintf(cash_line, sizeof(cash_line), "Cash:%s", buf);
    canvas_draw_str(canvas, 2, 54, cash_line);

    char shares_line[20];
    snprintf(shares_line, sizeof(shares_line), "Shr:%ld", (long)app->shares);
    canvas_draw_str(canvas, 2, 63, shares_line);

    format_cents(net_worth(app), buf, sizeof(buf));
    char worth_line[32];
    snprintf(worth_line, sizeof(worth_line), "Net:%s", buf);
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, worth_line);

    canvas_draw_str_aligned(canvas, 126, 54, AlignRight, AlignBottom, "<Sell Buy>");
}

static void draw_trade(Canvas* canvas, StockTycoonApp* app) {
    const char* title = app->trade_mode == TradeBuy ? "BUY SHARES" : "SELL SHARES";
    draw_header_bar(canvas, title);

    canvas_set_font(canvas, FontSecondary);
    char price_buf[24];
    format_cents(app->price_cents, price_buf, sizeof(price_buf));
    char pline[32];
    snprintf(pline, sizeof(pline), "Price: %s", price_buf);
    canvas_draw_str(canvas, 4, 24, pline);

    /* quantity selector */
    for(int i = 0; i < 3; i++) {
        int x = 14 + i * 36;
        int y = 40;
        bool sel = (uint8_t)i == app->trade_qty_index;
        if(sel) {
            canvas_draw_box(canvas, x - 12, y - 12, 26, 16);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_draw_frame(canvas, x - 12, y - 12, 26, 16);
        }
        char qbuf[4];
        snprintf(qbuf, sizeof(qbuf), "%d", TRADE_QTY[i]);
        canvas_draw_str_aligned(canvas, x, y, AlignCenter, AlignBottom, qbuf);
        canvas_set_color(canvas, ColorBlack);
    }

    int32_t qty = TRADE_QTY[app->trade_qty_index];
    int32_t total = qty * app->price_cents;
    char total_buf[24];
    format_cents(total, total_buf, sizeof(total_buf));
    char tline[32];
    snprintf(tline, sizeof(tline), "Total: %s", total_buf);
    canvas_draw_str(canvas, 4, 56, tline);

    if(app->message_ticks > 0) {
        canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, app->message);
    } else {
        canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "OK confirm");
    }
}

static void draw_confirm_quit(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignBottom, "Quit to menu?");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignBottom, "Progress will be lost");
    canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignBottom, "OK: Yes   BACK: No");
}

static void draw_game_over(Canvas* canvas, StockTycoonApp* app) {
    draw_header_bar(canvas, "Market Closed");
    canvas_set_font(canvas, FontSecondary);

    int32_t worth = net_worth(app);
    char buf[24];
    format_cents(worth, buf, sizeof(buf));
    char line[32];
    snprintf(line, sizeof(line), "Final: %s", buf);
    canvas_draw_str(canvas, 4, 26, line);

    int32_t profit = worth - app->start_net_worth_cents;
    format_cents(profit, buf, sizeof(buf));
    char pline[32];
    snprintf(pline, sizeof(pline), "P/L: %s", buf);
    canvas_draw_str(canvas, 4, 38, pline);

    if(app->new_high_score) {
        canvas_draw_str(canvas, 4, 50, "NEW HIGH SCORE!");
    } else {
        format_cents(app->best_net_worth_cents, buf, sizeof(buf));
        char hline[32];
        snprintf(hline, sizeof(hline), "Best: %s", buf);
        canvas_draw_str(canvas, 4, 50, hline);
    }

    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK: Menu");
}

static void draw_callback(Canvas* canvas, void* ctx) {
    StockTycoonApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    switch(app->state) {
    case GameStateSplash:
        draw_splash(canvas, app);
        break;
    case GameStateMenu:
        draw_menu(canvas, app);
        break;
    case GameStateHelp:
        draw_help(canvas, app);
        break;
    case GameStateSettings:
        draw_settings(canvas, app);
        break;
    case GameStatePlaying:
        draw_playing(canvas, app);
        break;
    case GameStateTrade:
        draw_trade(canvas, app);
        break;
    case GameStateConfirmQuit:
        draw_confirm_quit(canvas);
        break;
    case GameStateGameOver:
        draw_game_over(canvas, app);
        break;
    }

    furi_mutex_release(app->mutex);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    AppEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(queue, &event, FuriWaitForever);
}

static void handle_input_menu(StockTycoonApp* app, InputEvent* in) {
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return;
    if(in->key == InputKeyUp) {
        app->menu_index = (app->menu_index == 0) ? 3 : app->menu_index - 1;
    } else if(in->key == InputKeyDown) {
        app->menu_index = (app->menu_index + 1) % 4;
    } else if(in->key == InputKeyOk) {
        if(app->menu_index == 0) {
            start_new_game(app);
        } else if(app->menu_index == 1) {
            app->help_page = 0;
            app->state = GameStateHelp;
        } else if(app->menu_index == 2) {
            app->state = GameStateSettings;
        } else {
            app->help_page = 2; /* reuse help screen index for credits text */
            app->state = GameStateHelp;
        }
    } else if(in->key == InputKeyBack) {
        app->running = false;
    }
}

static void handle_input_help(StockTycoonApp* app, InputEvent* in) {
    if(in->type != InputTypeShort) return;
    if(in->key == InputKeyOk) {
        if(app->help_page == 2) {
            app->state = GameStateMenu;
        } else {
            app->help_page = (app->help_page + 1) % 2;
        }
    } else if(in->key == InputKeyBack) {
        app->state = GameStateMenu;
    }
}

static void handle_input_settings(StockTycoonApp* app, InputEvent* in) {
    if(in->type != InputTypeShort) return;
    if(in->key == InputKeyUp) {
        app->settings_row = 0;
    } else if(in->key == InputKeyDown) {
        app->settings_row = 1;
    } else if(in->key == InputKeyOk) {
        if(app->settings_row == 0) {
            app->sound_on = !app->sound_on;
            if(!app->sound_on && app->speaker_available && furi_hal_speaker_is_mine()) {
                furi_hal_speaker_stop();
            }
        } else {
            app->difficulty = (app->difficulty + 1) % 3;
        }
    } else if(in->key == InputKeyBack) {
        app->state = GameStateMenu;
    }
}

static void handle_input_playing(StockTycoonApp* app, InputEvent* in) {
    if(in->type != InputTypeShort) return;
    if(in->key == InputKeyRight) {
        app->trade_mode = TradeBuy;
        app->trade_qty_index = 0;
        app->message_ticks = 0;
        app->state = GameStateTrade;
    } else if(in->key == InputKeyLeft) {
        app->trade_mode = TradeSell;
        app->trade_qty_index = 0;
        app->message_ticks = 0;
        app->state = GameStateTrade;
    } else if(in->key == InputKeyBack) {
        app->state = GameStateConfirmQuit;
    }
}

static void handle_input_trade(StockTycoonApp* app, InputEvent* in) {
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return;
    if(in->key == InputKeyLeft) {
        app->trade_qty_index = (app->trade_qty_index == 0) ? 2 : app->trade_qty_index - 1;
    } else if(in->key == InputKeyRight) {
        app->trade_qty_index = (app->trade_qty_index + 1) % 3;
    } else if(in->key == InputKeyOk) {
        do_trade(app);
    } else if(in->key == InputKeyBack) {
        app->state = GameStatePlaying;
    }
}

static void handle_input_confirm_quit(StockTycoonApp* app, InputEvent* in) {
    if(in->type != InputTypeShort) return;
    if(in->key == InputKeyOk) {
        app->state = GameStateMenu;
    } else if(in->key == InputKeyBack) {
        app->state = GameStatePlaying;
    }
}

static void handle_input_game_over(StockTycoonApp* app, InputEvent* in) {
    if(in->type != InputTypeShort) return;
    if(in->key == InputKeyOk || in->key == InputKeyBack) {
        app->state = GameStateMenu;
    }
}

/* ------------------------------------------------------------------ */
/* Timer tick                                                          */
/* ------------------------------------------------------------------ */

static void timer_callback(void* ctx) {
    FuriMessageQueue* queue = ctx;
    AppEvent event = {.type = EventTypeTick};
    furi_message_queue_put(queue, &event, 0);
}

static void advance_music(StockTycoonApp* app) {
    if(!app->speaker_available) return;

    if(app->sfx_ticks_remaining > 0) {
        furi_hal_speaker_start(app->sfx_freq, 0.6f);
        app->sfx_ticks_remaining--;
        return;
    }

    if(!app->sound_on) {
        if(furi_hal_speaker_is_mine()) furi_hal_speaker_stop();
        return;
    }

    uint16_t freq = MELODY[app->music_step % MELODY_LEN];
    if(freq > 0) {
        furi_hal_speaker_start(freq, 0.25f);
    } else {
        furi_hal_speaker_stop();
    }
    app->music_step++;
}

static void on_tick(StockTycoonApp* app) {
    if(app->message_ticks > 0) app->message_ticks--;

    if(app->popup_ticks > 0) {
        app->popup_ticks--;
        app->popup_y_offset -= 1;
    }

    if(app->event_active) {
        if(app->event_ticks > 0) {
            app->event_ticks--;
        } else {
            app->event_active = false;
        }
    }

    if(app->state == GameStateSplash) {
        app->splash_ticks++;
    }

    if(app->state == GameStatePlaying) {
        app->sub_tick++;
        if(app->sub_tick >= TICKS_PER_ROUND) {
            app->sub_tick = 0;
            update_price(app);
            if(app->round >= ROUND_LIMIT) {
                end_game(app);
            }
        }
    }

    /* music/sfx advance every other tick for a slower tempo */
    static uint8_t music_div = 0;
    music_div++;
    if(music_div >= 2) {
        music_div = 0;
        advance_music(app);
    }
}

/* ------------------------------------------------------------------ */
/* App entry point                                                     */
/* ------------------------------------------------------------------ */

int32_t stock_tycoon_app(void* p) {
    UNUSED(p);

    StockTycoonApp* app = malloc(sizeof(StockTycoonApp));
    memset(app, 0, sizeof(StockTycoonApp));

    app->state = GameStateSplash;
    app->sound_on = true;
    app->difficulty = 1;
    app->running = true;
    app->best_net_worth_cents = load_high_score();

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(AppEvent));

    app->speaker_available = furi_hal_speaker_acquire(1000);

    app->notification = furi_record_open(RECORD_NOTIFICATION);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app->queue);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, app->queue);
    furi_timer_start(app->timer, furi_kernel_get_tick_frequency() / TICKS_PER_SEC);

    AppEvent event;
    while(app->running) {
        FuriStatus status = furi_message_queue_get(app->queue, &event, 100);
        if(status != FuriStatusOk) {
            continue;
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);

        if(event.type == EventTypeTick) {
            on_tick(app);
        } else if(event.type == EventTypeKey) {
            InputEvent* in = &event.input;

            if(in->key == InputKeyBack && in->type == InputTypeLong &&
               app->state != GameStatePlaying) {
                app->running = false;
            } else {
                switch(app->state) {
                case GameStateSplash:
                    if(in->type == InputTypeShort &&
                       (in->key == InputKeyOk || in->key == InputKeyBack)) {
                        app->state = GameStateMenu;
                    }
                    break;
                case GameStateMenu:
                    handle_input_menu(app, in);
                    break;
                case GameStateHelp:
                    handle_input_help(app, in);
                    break;
                case GameStateSettings:
                    handle_input_settings(app, in);
                    break;
                case GameStatePlaying:
                    handle_input_playing(app, in);
                    break;
                case GameStateTrade:
                    handle_input_trade(app, in);
                    break;
                case GameStateConfirmQuit:
                    handle_input_confirm_quit(app, in);
                    break;
                case GameStateGameOver:
                    handle_input_game_over(app, in);
                    break;
                }
            }
        }

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    /* cleanup */
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);

    if(app->speaker_available) {
        if(furi_hal_speaker_is_mine()) furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    view_port_free(app->view_port);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
