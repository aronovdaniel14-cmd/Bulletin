// Bulletin - minimalist daily news ticker for Flipper Zero + WiFi Dev Board
// Requires the FlipperHTTP firmware on the ESP32 dev board:
// https://github.com/jblanked/FlipperHTTP
//
// Feed: Hacker News front page via Algolia (no API key required).
// Change BULLETIN_FEED_URL below to point at any endpoint that returns
// JSON containing "title":"..." fields.

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/loading.h>
#include <storage/storage.h>

#include <flipper_http/flipper_http.h>

#define TAG "Bulletin"

#define BULLETIN_APP_DATA_PATH "/ext/apps_data/bulletin"
#define BULLETIN_WIFI_CFG_PATH BULLETIN_APP_DATA_PATH "/wifi.cfg"
#define BULLETIN_JSON_PATH BULLETIN_APP_DATA_PATH "/latest.json"

// Canonical HN Algolia query - extra Algolia-only params like
// attributesToRetrieve are rejected by the HN wrapper with HTTP 400.
#define BULLETIN_FEED_URL \
    "https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=15"

// Fallback if the primary query is rejected: latest stories by date
#define BULLETIN_FEED_URL_FALLBACK \
    "https://hn.algolia.com/api/v1/search_by_date?tags=story&hitsPerPage=15"

#define MAX_HEADLINES 15
#define HEADLINE_LEN 128
#define CRED_LEN 64


// View ids
typedef enum {
    BulletinViewMenu,
    BulletinViewTextInput,
    BulletinViewReader,
    BulletinViewWidget,
    BulletinViewLoading,
} BulletinViewId;

// Menu item ids
typedef enum {
    BulletinMenuRead,
    BulletinMenuSsid,
    BulletinMenuPassword,
    BulletinMenuConnect,
    BulletinMenuAbout,
} BulletinMenuId;

// Custom events
typedef enum {
    BulletinEventFetchOk,
    BulletinEventFetchFail,
} BulletinEvent;

// Granular fetch failure reasons
typedef enum {
    FetchErrNone,
    FetchErrNoBoard, // no PONG over UART
    FetchErrNoWifi, // board says WiFi not connected
    FetchErrSendFail, // UART write failed
    FetchErrNoStart, // request sent but [GET/SUCCESS] never came
    FetchErrTimeout, // transfer started but never finished
    FetchErrBoardError, // board reported [ERROR] ...
    FetchErrHttp, // HTTP status >= 400
    FetchErrParse, // response saved but no titles found
} FetchErr;

typedef struct BulletinApp BulletinApp;

typedef struct {
    BulletinApp* app;
} ReaderModel;

struct BulletinApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextInput* text_input;
    Widget* widget;
    Loading* loading;
    View* reader_view;

    FlipperHTTP* fhttp;
    FuriThread* fetch_thread;

    char ssid[CRED_LEN];
    char password[CRED_LEN];
    char input_buffer[CRED_LEN];
    bool editing_password;

    char headlines[MAX_HEADLINES][HEADLINE_LEN];
    int headline_count;
    int reader_index;

    FetchErr fetch_err;
    char diag[100]; // last line the board sent, for the failure screen
};

/* ---------------------------- config storage ---------------------------- */

static void bulletin_config_load(BulletinApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, BULLETIN_APP_DATA_PATH);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, BULLETIN_WIFI_CFG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[CRED_LEN * 2 + 4] = {0};
        size_t read = storage_file_read(file, buf, sizeof(buf) - 1);
        buf[read] = '\0';
        char* newline = strchr(buf, '\n');
        if(newline) {
            *newline = '\0';
            strlcpy(app->ssid, buf, CRED_LEN);
            char* pass = newline + 1;
            char* end = strchr(pass, '\n');
            if(end) *end = '\0';
            strlcpy(app->password, pass, CRED_LEN);
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void bulletin_config_save(BulletinApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, BULLETIN_APP_DATA_PATH);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, BULLETIN_WIFI_CFG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, app->ssid, strlen(app->ssid));
        storage_file_write(file, "\n", 1);
        storage_file_write(file, app->password, strlen(app->password));
        storage_file_write(file, "\n", 1);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* ------------------------------- parsing -------------------------------- */

// Extract an escaped JSON string starting at src (just after the opening
// quote) into out. Returns a pointer just past the closing quote, or NULL
// if no closing quote was found in src.
static const char* bulletin_extract_title(const char* src, char* out, size_t out_len) {
    size_t oi = 0;
    const char* p = src;
    while(*p && *p != '"') {
        char c = *p;
        if(c == '\\') {
            p++;
            if(*p == '\0') return NULL;
            char esc = *p;
            if(esc == 'n' || esc == 't' || esc == 'r') {
                c = ' ';
            } else if(esc == 'u') {
                for(int k = 0; k < 4 && *(p + 1) != '\0'; k++) p++;
                c = '?';
            } else {
                c = esc; // handles \" \\ \/
            }
        }
        if(oi < out_len - 1 && (unsigned char)c >= 32 && (unsigned char)c < 127) {
            out[oi++] = c;
        }
        p++;
    }
    if(*p != '"') return NULL;
    out[oi] = '\0';
    return p + 1;
}

// Stream the saved JSON in 1KB chunks and pull out up to MAX_HEADLINES
// "title" values - works no matter how large the response is.
static bool bulletin_parse_file(BulletinApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    int count = 0;

    if(storage_file_open(file, BULLETIN_JSON_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[1024 + 320 + 1]; // chunk + carry margin + NUL
        size_t carry = 0;
        bool eof = false;

        while(count < MAX_HEADLINES) {
            size_t read = 0;
            if(!eof) {
                read = storage_file_read(file, buf + carry, 1024);
                if(read == 0) eof = true;
            }
            size_t total = carry + read;
            if(total == 0) break;
            for(size_t i = 0; i < total; i++) {
                if(buf[i] == '\0') buf[i] = ' ';
            }
            buf[total] = '\0';

            size_t keep_from = total;
            const char* p = buf;
            while(count < MAX_HEADLINES) {
                const char* m = strstr(p, "\"title\":");
                if(!m) break;
                size_t moff = (size_t)(m - buf);
                // If the match sits too close to the chunk edge, the value
                // may be cut off - carry it into the next read instead.
                if(!eof && total - moff < 320) {
                    keep_from = moff;
                    break;
                }
                // tolerate whitespace between the colon and the value
                const char* v = m + 8;
                while(*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
                if(*v != '"') {
                    p = m + 8; // "title" maps to an object/null - skip it
                    continue;
                }
                const char* next =
                    bulletin_extract_title(v + 1, app->headlines[count], HEADLINE_LEN);
                if(!next) {
                    if(!eof) keep_from = moff;
                    break; // truncated at EOF: drop it
                }
                if(app->headlines[count][0] != '\0') count++;
                p = next;
            }

            if(eof) break;
            if(keep_from == total) {
                // keep a small overlap so a pattern split across the
                // boundary isn't missed
                keep_from = (total > 16) ? total - 16 : 0;
            }
            // never let the carry outgrow its margin (pathological values)
            if(total - keep_from > 320) keep_from = total - 320;
            carry = total - keep_from;
            memmove(buf, buf + keep_from, carry);
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    app->headline_count = count;
    app->reader_index = 0;
    return count > 0;
}

/* ----------------------------- fetch worker ------------------------------ */

static void bulletin_capture_diag(BulletinApp* app) {
    if(app->fhttp->last_response && app->fhttp->last_response[0] != '\0') {
        strlcpy(app->diag, app->fhttp->last_response, sizeof(app->diag));
    } else {
        app->diag[0] = '\0';
    }
}

static int32_t bulletin_fetch_worker(void* context) {
    BulletinApp* app = context;
    FlipperHTTP* fhttp = app->fhttp;
    app->fetch_err = FetchErrNone;
    app->diag[0] = '\0';
    bool ok = false;

    do {
        // 1) Make sure the board answers at all
        if(!flipper_http_send_command(fhttp, HTTP_CMD_PING)) {
            app->fetch_err = FetchErrNoBoard;
            break;
        }
        uint8_t tries = 50; // up to 5s - board may be waking up
        while(fhttp->state == INACTIVE && --tries > 0) {
            furi_delay_ms(100);
        }
        if(tries == 0) {
            app->fetch_err = FetchErrNoBoard;
            break;
        }
        furi_delay_ms(300);

        // 2) Ask the board if WiFi is actually connected ("true"/"false")
        if(fhttp->last_response) fhttp->last_response[0] = '\0';
        if(flipper_http_send_command(fhttp, HTTP_CMD_STATUS)) {
            bool wifi_known = false;
            bool wifi_up = false;
            for(uint8_t i = 0; i < 30; i++) { // up to 3s
                furi_delay_ms(100);
                if(!fhttp->last_response) continue;
                if(strstr(fhttp->last_response, "true")) {
                    wifi_known = true;
                    wifi_up = true;
                    break;
                }
                if(strstr(fhttp->last_response, "false")) {
                    wifi_known = true;
                    break;
                }
            }
            if(wifi_known && !wifi_up) {
                app->fetch_err = FetchErrNoWifi;
                break;
            }
            // unknown reply (older firmware) -> proceed anyway
        }

        // 3) Remove any stale edition so an old file can't mask a failure
        Storage* storage = furi_record_open(RECORD_STORAGE);
        storage_simply_remove(storage, BULLETIN_JSON_PATH);
        furi_record_close(RECORD_STORAGE);

        // 4) Request: try the primary URL, fall back to the alternate if
        // the server rejects it (4xx) or transport fails.
        const char* urls[2] = {BULLETIN_FEED_URL, BULLETIN_FEED_URL_FALLBACK};
        for(uint8_t attempt = 0; attempt < 2 && !ok; attempt++) {
            app->fetch_err = FetchErrNone;
            fhttp->state = IDLE;
            fhttp->started_receiving = false;
            fhttp->just_started = false;
            fhttp->save_received_data = true;
            fhttp->status_code = 0;
            snprintf(fhttp->file_path, sizeof(fhttp->file_path), BULLETIN_JSON_PATH);

            if(!flipper_http_request(
                   fhttp, GET, urls[attempt], "{\"Accept\":\"application/json\"}", NULL)) {
                app->fetch_err = FetchErrSendFail;
                continue;
            }
            fhttp->state = RECEIVING;

            // Phase 1: wait for [GET/SUCCESS] (started_receiving), NOT for
            // state==IDLE - stray UART lines and slow TLS handshakes can
            // flip the state early. Only ISSUE aborts.
            uint32_t waited = 0;
            while(!fhttp->started_receiving && fhttp->state != ISSUE && waited < 20000) {
                furi_delay_ms(100);
                waited += 100;
            }
            if(fhttp->state == ISSUE) {
                bulletin_capture_diag(app);
                app->fetch_err = FetchErrBoardError;
                furi_delay_ms(500);
                continue;
            }
            if(!fhttp->started_receiving) {
                bulletin_capture_diag(app);
                app->fetch_err = FetchErrNoStart;
                furi_delay_ms(500);
                continue;
            }

            // Phase 2: transfer is running - wait for [GET/END]
            // (started_receiving drops back to false)
            waited = 0;
            while(fhttp->started_receiving && fhttp->state != ISSUE && waited < 60000) {
                furi_delay_ms(100);
                waited += 100;
            }
            if(fhttp->state == ISSUE) {
                bulletin_capture_diag(app);
                app->fetch_err = FetchErrBoardError;
                furi_delay_ms(500);
                continue;
            }
            if(fhttp->started_receiving) {
                app->fetch_err = FetchErrTimeout;
                continue;
            }

            if(fhttp->status_code >= 400) {
                snprintf(app->diag, sizeof(app->diag), "HTTP status %d", fhttp->status_code);
                app->fetch_err = FetchErrHttp;
                furi_delay_ms(500);
                continue; // try the fallback URL
            }

            if(bulletin_parse_file(app)) {
                ok = true;
            } else {
                app->fetch_err = FetchErrParse;
            }
        }
    } while(false);

    view_dispatcher_send_custom_event(
        app->view_dispatcher, ok ? BulletinEventFetchOk : BulletinEventFetchFail);
    return 0;
}

/* ------------------------------ reader view ------------------------------ */

static void bulletin_reader_draw(Canvas* canvas, void* mctx) {
    ReaderModel* model = mctx;
    BulletinApp* app = model->app;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    char header[32];
    snprintf(
        header, sizeof(header), "Bulletin  %d/%d", app->reader_index + 1, app->headline_count);
    canvas_draw_str(canvas, 2, 10, header);
    canvas_draw_line(canvas, 0, 13, 127, 13);

    canvas_set_font(canvas, FontSecondary);

    if(app->headline_count == 0) {
        canvas_draw_str(canvas, 2, 30, "No headlines loaded.");
        return;
    }

    // Word wrap the headline to the screen width
    const char* text = app->headlines[app->reader_index];
    char line[40];
    size_t li = 0;
    uint8_t y = 24;
    const char* w = text;

    while(*w && y <= 54) {
        // find next word
        const char* start = w;
        while(*w && *w != ' ') w++;
        size_t wlen = (size_t)(w - start);
        if(wlen >= sizeof(line)) wlen = sizeof(line) - 1;

        // try appending word to the current line
        char candidate[40];
        if(li == 0) {
            memcpy(candidate, start, wlen);
            candidate[wlen] = '\0';
        } else {
            strlcpy(candidate, line, sizeof(candidate));
            strlcat(candidate, " ", sizeof(candidate));
            size_t clen = strlen(candidate);
            size_t room = sizeof(candidate) - 1 - clen;
            if(wlen > room) wlen = room;
            memcpy(candidate + clen, start, wlen);
            candidate[clen + wlen] = '\0';
        }

        if(canvas_string_width(canvas, candidate) <= 124) {
            strlcpy(line, candidate, sizeof(line));
            li = strlen(line);
        } else {
            // flush current line, start new one with this word
            canvas_draw_str(canvas, 2, y, line);
            y += 10;
            memcpy(line, start, wlen);
            line[wlen] = '\0';
            li = wlen;
        }
        while(*w == ' ') w++;
    }
    if(li > 0 && y <= 54) {
        canvas_draw_str(canvas, 2, y, line);
    }

    canvas_draw_str(canvas, 2, 63, "< prev");
    canvas_draw_str(canvas, 96, 63, "next >");
}

static bool bulletin_reader_input(InputEvent* event, void* context) {
    BulletinApp* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool consumed = false;
    if(event->key == InputKeyRight) {
        if(app->reader_index + 1 < app->headline_count) app->reader_index++;
        consumed = true;
    } else if(event->key == InputKeyLeft) {
        if(app->reader_index > 0) app->reader_index--;
        consumed = true;
    }

    if(consumed) {
        with_view_model(
            app->reader_view, ReaderModel * model, { UNUSED(model); }, true);
    }
    return consumed;
}

/* --------------------------- navigation helpers -------------------------- */

static uint32_t bulletin_nav_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t bulletin_nav_menu(void* context) {
    UNUSED(context);
    return BulletinViewMenu;
}

/* ------------------------------- callbacks ------------------------------- */

static void bulletin_show_message(BulletinApp* app, const char* title, const char* text) {
    widget_reset(app->widget);
    widget_add_string_element(app->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, title);
    widget_add_text_scroll_element(app->widget, 2, 14, 124, 50, text);
    view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewWidget);
}

static void bulletin_start_fetch(BulletinApp* app) {
    view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewLoading);
    furi_thread_start(app->fetch_thread);
}

static void bulletin_text_input_done(void* context) {
    BulletinApp* app = context;
    if(app->editing_password) {
        strlcpy(app->password, app->input_buffer, CRED_LEN);
    } else {
        strlcpy(app->ssid, app->input_buffer, CRED_LEN);
    }
    bulletin_config_save(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewMenu);
}

static void bulletin_menu_callback(void* context, uint32_t index) {
    BulletinApp* app = context;
    switch(index) {
    case BulletinMenuRead:
        bulletin_start_fetch(app);
        break;
    case BulletinMenuSsid:
        app->editing_password = false;
        strlcpy(app->input_buffer, app->ssid, CRED_LEN);
        text_input_set_header_text(app->text_input, "WiFi SSID");
        text_input_set_result_callback(
            app->text_input, bulletin_text_input_done, app, app->input_buffer, CRED_LEN, false);
        view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewTextInput);
        break;
    case BulletinMenuPassword:
        app->editing_password = true;
        strlcpy(app->input_buffer, app->password, CRED_LEN);
        text_input_set_header_text(app->text_input, "WiFi Password");
        text_input_set_result_callback(
            app->text_input, bulletin_text_input_done, app, app->input_buffer, CRED_LEN, false);
        view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewTextInput);
        break;
    case BulletinMenuConnect:
        if(strlen(app->ssid) == 0) {
            bulletin_show_message(app, "No SSID", "Set your WiFi SSID first.");
            break;
        }
        if(flipper_http_save_wifi(app->fhttp, app->ssid, app->password)) {
            furi_delay_ms(500);
            flipper_http_send_command(app->fhttp, HTTP_CMD_WIFI_CONNECT);
            bulletin_show_message(
                app,
                "WiFi Sent",
                "Credentials saved to board.\nGive it a few seconds to\nconnect, then Read News.");
        } else {
            bulletin_show_message(
                app, "Board Error", "Could not talk to the\nWiFi dev board over UART.");
        }
        break;
    case BulletinMenuAbout:
        bulletin_show_message(
            app,
            "Bulletin v1.2",
            "Daily headlines over the\nWiFi dev board running\nFlipperHTTP firmware.\nFeed: HN front page.");
        break;
    default:
        break;
    }
}

static bool bulletin_custom_event_callback(void* context, uint32_t event) {
    BulletinApp* app = context;
    switch(event) {
    case BulletinEventFetchOk:
        furi_thread_join(app->fetch_thread);
        view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewReader);
        with_view_model(
            app->reader_view, ReaderModel * model, { UNUSED(model); }, true);
        return true;
    case BulletinEventFetchFail: {
        furi_thread_join(app->fetch_thread);
        const char* title = "Fetch Failed";
        const char* hint = "Unknown error.";
        switch(app->fetch_err) {
        case FetchErrNoBoard:
            title = "No Board";
            hint = "No reply over UART. Check\nthe dev board is attached\nand flashed w/ FlipperHTTP.";
            break;
        case FetchErrNoWifi:
            title = "WiFi Down";
            hint = "Board reports WiFi not\nconnected. Send WiFi to\nBoard, wait ~10s, retry.";
            break;
        case FetchErrSendFail:
            title = "UART Error";
            hint = "Could not send the request\nto the board.";
            break;
        case FetchErrNoStart:
            title = "No Response";
            hint = "Request sent, but the board\nnever started the transfer.";
            break;
        case FetchErrTimeout:
            title = "Timed Out";
            hint = "Transfer started but never\nfinished. Weak WiFi?";
            break;
        case FetchErrBoardError:
            title = "Board Error";
            hint = "The board reported an\nerror (see below).";
            break;
        case FetchErrHttp:
            title = "HTTP Error";
            hint = "Server rejected the request\n(see below).";
            break;
        case FetchErrParse:
            title = "Parse Failed";
            hint = "Response saved but no\ntitles found. Check\nlatest.json in apps_data.";
            break;
        default:
            break;
        }
        char text[220];
        if(app->diag[0] != '\0') {
            snprintf(text, sizeof(text), "%s\n---\n%s", hint, app->diag);
        } else {
            snprintf(text, sizeof(text), "%s", hint);
        }
        bulletin_show_message(app, title, text);
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------ app lifecycle ---------------------------- */

static BulletinApp* bulletin_app_alloc(void) {
    BulletinApp* app = malloc(sizeof(BulletinApp));
    memset(app, 0, sizeof(BulletinApp));

    bulletin_config_load(app);

    app->fhttp = flipper_http_alloc();

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, bulletin_custom_event_callback);

    // Main menu
    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "Bulletin");
    submenu_add_item(app->submenu, "Read News", BulletinMenuRead, bulletin_menu_callback, app);
    submenu_add_item(app->submenu, "Set WiFi SSID", BulletinMenuSsid, bulletin_menu_callback, app);
    submenu_add_item(
        app->submenu, "Set WiFi Password", BulletinMenuPassword, bulletin_menu_callback, app);
    submenu_add_item(
        app->submenu, "Send WiFi to Board", BulletinMenuConnect, bulletin_menu_callback, app);
    submenu_add_item(app->submenu, "About", BulletinMenuAbout, bulletin_menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), bulletin_nav_exit);
    view_dispatcher_add_view(
        app->view_dispatcher, BulletinViewMenu, submenu_get_view(app->submenu));

    // Text input
    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), bulletin_nav_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, BulletinViewTextInput, text_input_get_view(app->text_input));

    // Widget (messages / about)
    app->widget = widget_alloc();
    view_set_previous_callback(widget_get_view(app->widget), bulletin_nav_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, BulletinViewWidget, widget_get_view(app->widget));

    // Loading spinner
    app->loading = loading_alloc();
    view_set_previous_callback(loading_get_view(app->loading), bulletin_nav_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, BulletinViewLoading, loading_get_view(app->loading));

    // Reader
    app->reader_view = view_alloc();
    view_allocate_model(app->reader_view, ViewModelTypeLocking, sizeof(ReaderModel));
    with_view_model(
        app->reader_view, ReaderModel * model, { model->app = app; }, false);
    view_set_context(app->reader_view, app);
    view_set_draw_callback(app->reader_view, bulletin_reader_draw);
    view_set_input_callback(app->reader_view, bulletin_reader_input);
    view_set_previous_callback(app->reader_view, bulletin_nav_menu);
    view_dispatcher_add_view(app->view_dispatcher, BulletinViewReader, app->reader_view);

    // Fetch worker thread (started on demand)
    app->fetch_thread =
        furi_thread_alloc_ex("BulletinFetch", 2048, bulletin_fetch_worker, app);

    return app;
}

static void bulletin_app_free(BulletinApp* app) {
    furi_thread_free(app->fetch_thread);

    view_dispatcher_remove_view(app->view_dispatcher, BulletinViewMenu);
    submenu_free(app->submenu);
    view_dispatcher_remove_view(app->view_dispatcher, BulletinViewTextInput);
    text_input_free(app->text_input);
    view_dispatcher_remove_view(app->view_dispatcher, BulletinViewWidget);
    widget_free(app->widget);
    view_dispatcher_remove_view(app->view_dispatcher, BulletinViewLoading);
    loading_free(app->loading);
    view_dispatcher_remove_view(app->view_dispatcher, BulletinViewReader);
    view_free(app->reader_view);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    if(app->fhttp) flipper_http_free(app->fhttp);
    free(app);
}

int32_t bulletin_app(void* p) {
    UNUSED(p);
    BulletinApp* app = bulletin_app_alloc();

    if(!app->fhttp) {
        FURI_LOG_E(TAG, "Failed to init FlipperHTTP UART");
        bulletin_app_free(app);
        return -1;
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    bulletin_app_free(app);
    return 0;
}
