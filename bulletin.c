// Bulletin v2.2 - minimalist news reader for Flipper Zero + WiFi Dev Board
// Requires the FlipperHTTP firmware on the ESP32 dev board:
// https://github.com/jblanked/FlipperHTTP
//
// Daily Edition: HN top stories (official Firebase API, two-stage fetch).
// Search: HN (Algolia) + Reddit, merged, with author + date on every item.
// OK on a headline: full article text via the r.jina.ai reader proxy.
// OK in an article: first in-article photo, resized server-side by
// wsrv.nl to 112x52 JPEG, decoded with tjpgd and Bayer-dithered to 1-bit.

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/loading.h>
#include <storage/storage.h>

#include <flipper_http/flipper_http.h>
#include "tjpgd.h"

#define TAG "Bulletin"

#define BULLETIN_APP_DATA_PATH "/ext/apps_data/bulletin"
#define BULLETIN_WIFI_CFG_PATH BULLETIN_APP_DATA_PATH "/wifi.cfg"
#define BULLETIN_JSON_PATH BULLETIN_APP_DATA_PATH "/latest.json"
#define BULLETIN_IDS_PATH BULLETIN_APP_DATA_PATH "/topstories.json"
#define BULLETIN_ARTICLE_PATH BULLETIN_APP_DATA_PATH "/article.md"
#define BULLETIN_PHOTO_PATH BULLETIN_APP_DATA_PATH "/photo.jpg"

// Daily: official HN Firebase API
#define BULLETIN_TOPSTORIES_URL "https://hacker-news.firebaseio.com/v0/topstories.json"
#define BULLETIN_ITEM_URL_FMT "https://hacker-news.firebaseio.com/v0/item/%lu.json"
#define BULLETIN_ITEMS 10

// Search sources (%s = URL-encoded query)
#define BULLETIN_SEARCH_HN_FMT \
    "https://hn.algolia.com/api/v1/search?tags=story&hitsPerPage=5&query=%s"
#define BULLETIN_SEARCH_REDDIT_FMT \
    "https://www.reddit.com/search.json?q=%s&limit=5&raw_json=1"

// Article reader proxy (any URL -> markdown/plain text)
// Reader proxy endpoint (POSTed to with {"url": target})
#define BULLETIN_READER_URL "https://r.jina.ai/"

// Image proxy: server-side resize to the Flipper screen, tiny JPEG out
#define BULLETIN_PHOTO_W 112
#define BULLETIN_PHOTO_H 52
#define BULLETIN_IMGPROXY_FMT \
    "https://wsrv.nl/?url=%s&w=112&h=52&fit=cover&output=jpg&q=50"

#define MAX_HEADLINES 15
#define TITLE_LEN 128
#define AUTHOR_LEN 24
#define DATE_LEN 12
#define SOURCE_LEN 8
#define LINK_LEN 160
#define CRED_LEN 64
#define QUERY_LEN 40
#define PHOTO_URL_LEN 200
#define PHOTO_URL_ENC_LEN 384

#define ARTICLE_MAX_BYTES (6 * 1024)
#define ARTICLE_LINE_CHARS 26
#define ARTICLE_MAX_LINES 240
#define ARTICLE_VISIBLE_LINES 4

#define PHOTO_STRIDE ((BULLETIN_PHOTO_W + 7) / 8)
#define PHOTO_BMP_SIZE (PHOTO_STRIDE * BULLETIN_PHOTO_H)

// View ids
typedef enum {
    BulletinViewMenu,
    BulletinViewTextInput,
    BulletinViewReader,
    BulletinViewWidget,
    BulletinViewLoading,
    BulletinViewArticle,
    BulletinViewPhoto,
} BulletinViewId;

// Menu item ids
typedef enum {
    BulletinMenuRead,
    BulletinMenuSearch,
    BulletinMenuSsid,
    BulletinMenuPassword,
    BulletinMenuConnect,
    BulletinMenuAbout,
} BulletinMenuId;

// What the shared TextInput is currently editing
typedef enum {
    InputSsid,
    InputPassword,
    InputSearch,
} InputKind;

// Custom events
typedef enum {
    BulletinEventFetchOk, // headlines ready -> reader
    BulletinEventArticleOk, // article text ready -> article view
    BulletinEventPhotoOk, // photo decoded -> photo view
    BulletinEventFetchFail, // any mode failed -> message
} BulletinEvent;

// Fetch mode
typedef enum {
    FetchModeDaily,
    FetchModeSearch,
    FetchModeArticle,
    FetchModePhoto,
} FetchMode;

// Granular fetch failure reasons
typedef enum {
    FetchErrNone,
    FetchErrNoBoard,
    FetchErrNoWifi,
    FetchErrSendFail,
    FetchErrNoStart,
    FetchErrTimeout,
    FetchErrBoardError,
    FetchErrHttp,
    FetchErrParse,
    FetchErrPhotoDecode,
} FetchErr;

// Field extraction modes for the generic scanner
typedef enum {
    FieldQuoted, // "key":"string value"
    FieldIsoDate, // "key":"2024-03-29T16:16:50Z" -> "2024-03-29"
    FieldEpochDate, // "key":1721366773(.0) -> "2024-07-19"
} FieldMode;

typedef struct {
    char title[TITLE_LEN];
    char author[AUTHOR_LEN];
    char date[DATE_LEN];
    char source[SOURCE_LEN];
    char link[LINK_LEN];
} Headline;

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
    View* article_view;
    View* photo_view;

    FlipperHTTP* fhttp;
    FuriThread* fetch_thread;

    char ssid[CRED_LEN];
    char password[CRED_LEN];
    char query[QUERY_LEN];
    char input_buffer[CRED_LEN];
    InputKind input_kind;

    Headline items[MAX_HEADLINES];
    int headline_count;
    int reader_index;
    int detail_index; // which headline the article/photo belongs to

    // Article state
    char (*article_lines)[ARTICLE_LINE_CHARS + 2];
    int article_line_count;
    int article_scroll;
    char photo_url[PHOTO_URL_LEN];

    // Photo state
    uint8_t photo_bmp[PHOTO_BMP_SIZE];
    uint16_t photo_w;
    uint16_t photo_h;

    FetchMode fetch_mode;
    FetchErr fetch_err;
    char diag[100];
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

/* ------------------------------- helpers -------------------------------- */

// Unix epoch -> "YYYY-MM-DD" (Howard Hinnant's civil-from-days)
static void bulletin_epoch_to_date(uint32_t epoch, char* out, size_t out_len) {
    int64_t z = (int64_t)(epoch / 86400) + 719468;
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    snprintf(out, out_len, "%04ld-%02ld-%02ld", (long)y, (long)m, (long)d);
}

// Loose query encoding: keep [A-Za-z0-9-_.], space -> %20, drop the rest
static void bulletin_url_encode_query(const char* in, char* out, size_t out_len) {
    size_t oi = 0;
    for(const char* p = in; *p && oi + 4 < out_len; p++) {
        char c = *p;
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.') {
            out[oi++] = c;
        } else if(c == ' ') {
            out[oi++] = '%';
            out[oi++] = '2';
            out[oi++] = '0';
        }
    }
    out[oi] = '\0';
}

// Strict %XX encoding for embedding a full URL as a query parameter
static void bulletin_url_encode_full(const char* in, char* out, size_t out_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t oi = 0;
    for(const char* p = in; *p && oi + 4 < out_len; p++) {
        char c = *p;
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~') {
            out[oi++] = c;
        } else {
            out[oi++] = '%';
            out[oi++] = hex[((unsigned char)c) >> 4];
            out[oi++] = hex[((unsigned char)c) & 0x0F];
        }
    }
    out[oi] = '\0';
}

/* ------------------------------- parsing -------------------------------- */

// Extract an escaped JSON string starting at src (just after the opening
// quote) into out. Returns a pointer just past the closing quote, or NULL
// if no closing quote was found in src.
static const char* bulletin_extract_string(const char* src, char* out, size_t out_len) {
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

// Stream a saved JSON file in 1KB chunks and pull out values for `key`,
// writing them into out + i*stride, starting at index `start`, up to
// `max_total`. Returns the new total count. Buffer lives on the heap.
static int bulletin_scan_field(
    const char* path,
    const char* key,
    FieldMode mode,
    char* out,
    size_t stride,
    size_t field_len,
    int start,
    int max_total) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    int count = start;

    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    size_t pat_len = strlen(pattern);

    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const size_t buf_size = 1024 + 320 + 1;
        char* buf = malloc(buf_size);
        size_t carry = 0;
        bool eof = false;

        while(count < max_total) {
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
            while(count < max_total) {
                const char* m = strstr(p, pattern);
                if(!m) break;
                size_t moff = (size_t)(m - buf);
                if(!eof && total - moff < 320) {
                    keep_from = moff;
                    break;
                }
                const char* v = m + pat_len;
                while(*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
                char* slot = out + (size_t)count * stride;

                if(mode == FieldEpochDate) {
                    if(*v < '0' || *v > '9') {
                        p = m + pat_len;
                        continue;
                    }
                    uint32_t epoch = 0;
                    while(*v >= '0' && *v <= '9') {
                        epoch = epoch * 10 + (uint32_t)(*v - '0');
                        v++;
                    }
                    bulletin_epoch_to_date(epoch, slot, field_len);
                    count++;
                    p = v;
                } else {
                    if(*v != '"') {
                        p = m + pat_len;
                        continue;
                    }
                    const char* next = bulletin_extract_string(v + 1, slot, field_len);
                    if(!next) {
                        if(!eof) keep_from = moff;
                        break;
                    }
                    if(mode == FieldIsoDate && strlen(slot) > 10) {
                        slot[10] = '\0';
                    }
                    if(slot[0] != '\0') count++;
                    p = next;
                }
            }

            if(eof) break;
            if(keep_from == total) {
                keep_from = (total > 24) ? total - 24 : 0;
            }
            if(total - keep_from > 320) keep_from = total - 320;
            carry = total - keep_from;
            memmove(buf, buf + keep_from, carry);
        }
        free(buf);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return count;
}

// Three aligned passes (title/author/date) over one source's saved
// response; appends complete Headline entries. Returns new total.
static int bulletin_scan_source(
    BulletinApp* app,
    const char* path,
    const char* source,
    const char* title_key,
    const char* author_key,
    const char* date_key,
    FieldMode date_mode,
    int start,
    int max_total) {
    if(max_total > MAX_HEADLINES) max_total = MAX_HEADLINES;
    if(start >= max_total) return start;

    int nt = bulletin_scan_field(
        path, title_key, FieldQuoted, app->items[0].title, sizeof(Headline), TITLE_LEN, start,
        max_total);
    int na = bulletin_scan_field(
        path, author_key, FieldQuoted, app->items[0].author, sizeof(Headline), AUTHOR_LEN, start,
        max_total);
    int nd = bulletin_scan_field(
        path, date_key, date_mode, app->items[0].date, sizeof(Headline), DATE_LEN, start,
        max_total);

    int n = nt;
    if(na < n) n = na;
    if(nd < n) n = nd;
    for(int i = start; i < n; i++) {
        strlcpy(app->items[i].source, source, SOURCE_LEN);
    }
    return n;
}

// Parse the leading integers out of topstories.json ("[123,456,...]").
static int bulletin_parse_ids(const char* path, uint32_t* ids, int max_ids) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    int count = 0;

    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[256];
        size_t read;
        uint32_t cur = 0;
        bool in_num = false;
        while(count < max_ids && (read = storage_file_read(file, buf, sizeof(buf))) > 0) {
            for(size_t i = 0; i < read && count < max_ids; i++) {
                char c = buf[i];
                if(c >= '0' && c <= '9') {
                    cur = cur * 10 + (uint32_t)(c - '0');
                    in_num = true;
                } else if(in_num) {
                    ids[count++] = cur;
                    cur = 0;
                    in_num = false;
                }
            }
        }
        if(in_num && count < max_ids) ids[count++] = cur;
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return count;
}

/* ---------------------------- article handling --------------------------- */

// Load the saved r.jina.ai markdown, extract the first in-article image
// URL, strip markdown link/image syntax, and word-wrap into fixed lines
// for the pager view. Returns true if any text was produced.
static bool bulletin_prepare_article(BulletinApp* app) {
    app->article_line_count = 0;
    app->article_scroll = 0;
    app->photo_url[0] = '\0';

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    char* raw = NULL;
    size_t raw_len = 0;

    if(storage_file_open(file, BULLETIN_ARTICLE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        raw = malloc(ARTICLE_MAX_BYTES + 1);
        raw_len = storage_file_read(file, raw, ARTICLE_MAX_BYTES);
        raw[raw_len] = '\0';
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(!raw || raw_len == 0) {
        if(raw) free(raw);
        return false;
    }

    // Skip the jina preamble if present ("Markdown Content:\n")
    const char* src = raw;
    const char* mc = strstr(raw, "Markdown Content:");
    if(mc) src = mc + strlen("Markdown Content:");

    // Pass 1: find the first image URL anywhere in the raw text
    const char* img = strstr(raw, "![");
    while(img) {
        const char* open = strstr(img, "](");
        if(!open) break;
        const char* close = strchr(open + 2, ')');
        if(!close) break;
        size_t ulen = (size_t)(close - (open + 2));
        if(ulen > 8 && ulen < PHOTO_URL_LEN && strncmp(open + 2, "http", 4) == 0) {
            memcpy(app->photo_url, open + 2, ulen);
            app->photo_url[ulen] = '\0';
            break;
        }
        img = strstr(close, "![");
    }

    // Pass 2: sanitize into clean text (drop image tags, unwrap links)
    char* clean = malloc(ARTICLE_MAX_BYTES + 1);
    size_t ci = 0;
    int nl_run = 0;
    for(const char* p = src; *p && ci < ARTICLE_MAX_BYTES; p++) {
        // image tag: skip "![...](...)" entirely
        if(p[0] == '!' && p[1] == '[') {
            const char* close = strchr(p, ')');
            if(close) {
                p = close;
                continue;
            }
        }
        // link: keep the text, skip "(url)"
        if(p[0] == ']' && p[1] == '(') {
            const char* close = strchr(p + 2, ')');
            if(close) {
                p = close;
                continue;
            }
        }
        char c = *p;
        if(c == '[' || c == '#' || c == '*' || c == '`') continue;
        if(c == '\n') {
            if(++nl_run > 2) continue;
        } else {
            nl_run = 0;
        }
        if(c == '\t') c = ' ';
        if(c == '\n' || ((unsigned char)c >= 32 && (unsigned char)c < 127)) {
            clean[ci++] = c;
        }
    }
    clean[ci] = '\0';
    free(raw);

    // Pass 3: word-wrap into fixed-width lines
    if(!app->article_lines) {
        app->article_lines = malloc((size_t)ARTICLE_MAX_LINES * (ARTICLE_LINE_CHARS + 2));
    }
    int lines = 0;
    const char* p = clean;
    while(*p && lines < ARTICLE_MAX_LINES) {
        // skip leading spaces
        while(*p == ' ') p++;
        if(*p == '\0') break;
        if(*p == '\n') {
            p++;
            continue;
        }
        char* line = app->article_lines[lines];
        size_t li = 0;
        const char* last_space = NULL;
        size_t last_space_li = 0;
        const char* q = p;
        while(*q && *q != '\n' && li < ARTICLE_LINE_CHARS) {
            if(*q == ' ') {
                last_space = q;
                last_space_li = li;
            }
            line[li++] = *q++;
        }
        if(*q && *q != '\n' && last_space && last_space_li > 0) {
            // break at the last space
            li = last_space_li;
            q = last_space + 1;
        }
        line[li] = '\0';
        if(li > 0) lines++;
        p = (*q == '\n') ? q + 1 : q;
    }
    free(clean);

    app->article_line_count = lines;
    return lines > 0;
}

/* ----------------------------- photo handling ---------------------------- */

typedef struct {
    File* file;
    BulletinApp* app;
} JpegCtx;

static size_t bulletin_jpeg_in(JDEC* jd, uint8_t* buf, size_t n) {
    JpegCtx* ctx = (JpegCtx*)jd->device;
    if(buf) {
        return storage_file_read(ctx->file, buf, n);
    }
    // skip n bytes
    uint64_t pos = storage_file_tell(ctx->file);
    storage_file_seek(ctx->file, pos + n, true);
    return n;
}

// Bayer 4x4 ordered dithering matrix (thresholds scaled to 0..255)
static const uint8_t bayer4[4][4] = {
    {15, 135, 45, 165},
    {195, 75, 225, 105},
    {60, 180, 30, 150},
    {240, 120, 210, 90},
};

static int bulletin_jpeg_out(JDEC* jd, void* bitmap, JRECT* rect) {
    JpegCtx* ctx = (JpegCtx*)jd->device;
    BulletinApp* app = ctx->app;
    const uint8_t* px = (const uint8_t*)bitmap; // RGB888 (JD_FORMAT 0)

    for(uint16_t y = rect->top; y <= rect->bottom; y++) {
        for(uint16_t x = rect->left; x <= rect->right; x++) {
            uint16_t gray =
                (uint16_t)((px[0] * 299u + px[1] * 587u + px[2] * 114u) / 1000u);
            px += 3;
            if(x >= BULLETIN_PHOTO_W || y >= BULLETIN_PHOTO_H) continue;
            if(gray < bayer4[y & 3][x & 3]) {
                app->photo_bmp[y * PHOTO_STRIDE + (x >> 3)] |= (uint8_t)(1 << (x & 7));
            }
        }
    }
    return 1; // continue decoding
}

static bool bulletin_decode_photo(BulletinApp* app) {
    memset(app->photo_bmp, 0, sizeof(app->photo_bmp));
    app->photo_w = 0;
    app->photo_h = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = false;

    if(storage_file_open(file, BULLETIN_PHOTO_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        void* work = malloc(3800); // tjpgd workspace (JD_FASTDECODE 0)
        JDEC jdec;
        JpegCtx ctx = {.file = file, .app = app};
        JRESULT res = jd_prepare(&jdec, bulletin_jpeg_in, work, 3800, &ctx);
        if(res == JDR_OK && jdec.width <= 256 && jdec.height <= 256) {
            res = jd_decomp(&jdec, bulletin_jpeg_out, 0);
            if(res == JDR_OK) {
                app->photo_w =
                    (jdec.width > BULLETIN_PHOTO_W) ? BULLETIN_PHOTO_W : jdec.width;
                app->photo_h =
                    (jdec.height > BULLETIN_PHOTO_H) ? BULLETIN_PHOTO_H : jdec.height;
                ok = true;
            }
        }
        free(work);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* ----------------------------- fetch worker ------------------------------ */

static void bulletin_capture_diag(BulletinApp* app) {
    if(app->fhttp->last_response && app->fhttp->last_response[0] != '\0') {
        strlcpy(app->diag, app->fhttp->last_response, sizeof(app->diag));
    } else {
        app->diag[0] = '\0';
    }
}

// One request (GET, POST, or BYTES) saved to a file, with the
// marker-based two-phase wait.
static FetchErr bulletin_do_request(
    BulletinApp* app,
    HTTPMethod method,
    const char* url,
    const char* payload,
    const char* save_path) {
    FlipperHTTP* fhttp = app->fhttp;
    fhttp->state = IDLE;
    fhttp->started_receiving = false;
    fhttp->just_started = false;
    fhttp->save_received_data = (method != BYTES && method != BYTES_POST);
    fhttp->status_code = 0;
    snprintf(fhttp->file_path, sizeof(fhttp->file_path), "%s", save_path);

    const char* headers = (method == POST) ?
                              "{\"Content-Type\":\"application/json\",\"Accept\":\"text/plain\"}" :
                              "{\"Accept\":\"*/*\"}";
    if(!flipper_http_request(fhttp, method, url, headers, payload)) {
        return FetchErrSendFail;
    }
    fhttp->state = RECEIVING;

    // Phase 1: wait for [GET/SUCCESS] (started_receiving), NOT for
    // state==IDLE - stray UART lines and slow TLS handshakes can flip the
    // state early. Only ISSUE aborts.
    uint32_t waited = 0;
    while(!fhttp->started_receiving && fhttp->state != ISSUE && waited < 20000) {
        furi_delay_ms(100);
        waited += 100;
    }
    if(fhttp->state == ISSUE) {
        bulletin_capture_diag(app);
        furi_delay_ms(300);
        return FetchErrBoardError;
    }
    if(!fhttp->started_receiving) {
        bulletin_capture_diag(app);
        return FetchErrNoStart;
    }

    // Phase 2: transfer running - wait for [GET/END]
    waited = 0;
    while(fhttp->started_receiving && fhttp->state != ISSUE && waited < 60000) {
        furi_delay_ms(100);
        waited += 100;
    }
    if(fhttp->state == ISSUE) {
        bulletin_capture_diag(app);
        furi_delay_ms(300);
        return FetchErrBoardError;
    }
    if(fhttp->started_receiving) {
        return FetchErrTimeout;
    }
    if(fhttp->status_code >= 400) {
        snprintf(app->diag, sizeof(app->diag), "HTTP status %d", fhttp->status_code);
        return FetchErrHttp;
    }
    return FetchErrNone;
}

static FetchErr
    bulletin_do_get(BulletinApp* app, const char* url, const char* save_path, bool bytes) {
    return bulletin_do_request(app, bytes ? BYTES : GET, url, NULL, save_path);
}

// The FlipperHTTP firmware injects a "{}" body into every GET, which
// strict CDNs (Fastly/Reddit, Algolia, jina) reject with HTTP 400 -
// that is why direct GETs to those hosts fail while Firebase works.
// Workaround without reflashing the board: POST to the r.jina.ai
// reader with {"url": target}. POST carries OUR payload, so nothing
// is injected, and jina performs a clean GET to the target for us.
static FetchErr
    bulletin_fetch_via_reader(BulletinApp* app, const char* target, const char* save_path) {
    char payload[LINK_LEN + 224];
    snprintf(payload, sizeof(payload), "{\"url\":\"%s\"}", target);
    return bulletin_do_request(app, POST, BULLETIN_READER_URL, payload, save_path);
}

// Board liveness + WiFi status pre-checks shared by all modes.
static FetchErr bulletin_preflight(BulletinApp* app) {
    FlipperHTTP* fhttp = app->fhttp;

    if(!flipper_http_send_command(fhttp, HTTP_CMD_PING)) return FetchErrNoBoard;
    uint8_t tries = 50;
    while(fhttp->state == INACTIVE && --tries > 0) {
        furi_delay_ms(100);
    }
    if(tries == 0) return FetchErrNoBoard;
    furi_delay_ms(300);

    if(fhttp->last_response) fhttp->last_response[0] = '\0';
    if(flipper_http_send_command(fhttp, HTTP_CMD_STATUS)) {
        bool wifi_known = false;
        bool wifi_up = false;
        for(uint8_t i = 0; i < 30; i++) {
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
        if(wifi_known && !wifi_up) return FetchErrNoWifi;
    }
    return FetchErrNone;
}

static void bulletin_remove_file(const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_remove(storage, path);
    furi_record_close(RECORD_STORAGE);
}

static int32_t bulletin_fetch_worker(void* context) {
    BulletinApp* app = context;
    app->fetch_err = FetchErrNone;
    app->diag[0] = '\0';
    BulletinEvent ok_event = BulletinEventFetchOk;
    bool ok = false;

    do {
        FetchErr pre = bulletin_preflight(app);
        if(pre != FetchErrNone) {
            app->fetch_err = pre;
            break;
        }

        if(app->fetch_mode == FetchModeArticle) {
            // ---- Full article text via r.jina.ai ----
            ok_event = BulletinEventArticleOk;
            const Headline* h = &app->items[app->detail_index];
            if(h->link[0] == '\0') {
                app->fetch_err = FetchErrParse;
                break;
            }
            bulletin_remove_file(BULLETIN_ARTICLE_PATH);
            FetchErr err = bulletin_fetch_via_reader(app, h->link, BULLETIN_ARTICLE_PATH);
            if(err != FetchErrNone) {
                app->fetch_err = err;
                break;
            }
            if(bulletin_prepare_article(app)) {
                ok = true;
            } else {
                app->fetch_err = FetchErrParse;
            }
        } else if(app->fetch_mode == FetchModePhoto) {
            // ---- First in-article photo, resized server-side ----
            ok_event = BulletinEventPhotoOk;
            if(app->photo_url[0] == '\0') {
                app->fetch_err = FetchErrParse;
                break;
            }
            bulletin_remove_file(BULLETIN_PHOTO_PATH);
            char enc[PHOTO_URL_ENC_LEN];
            // wsrv accepts scheme-less source URLs; saves command space
            const char* src = app->photo_url;
            if(strncmp(src, "https://", 8) == 0) src += 8;
            else if(strncmp(src, "http://", 7) == 0) src += 7;
            bulletin_url_encode_full(src, enc, sizeof(enc));
            char url[PHOTO_URL_ENC_LEN + 80];
            snprintf(url, sizeof(url), BULLETIN_IMGPROXY_FMT, enc);
            if(strlen(url) > 460) { // must fit the board's command buffer
                app->fetch_err = FetchErrParse;
                break;
            }
            FetchErr err = bulletin_do_get(app, url, BULLETIN_PHOTO_PATH, true);
            if(err != FetchErrNone) {
                app->fetch_err = err;
                break;
            }
            if(bulletin_decode_photo(app)) {
                ok = true;
            } else {
                app->fetch_err = FetchErrPhotoDecode;
            }
        } else if(app->fetch_mode == FetchModeSearch) {
            // ---- Search: HN + Reddit, merged ----
            bulletin_remove_file(BULLETIN_JSON_PATH);
            memset(app->items, 0, sizeof(app->items));
            app->headline_count = 0;
            int count = 0;

            char encq[QUERY_LEN * 3 + 1];
            bulletin_url_encode_query(app->query, encq, sizeof(encq));
            if(encq[0] == '\0') {
                app->fetch_err = FetchErrParse;
                break;
            }
            char url[192];
            FetchErr first_err = FetchErrNone;

            // Source 1: HN via Algolia (tunneled through the reader)
            snprintf(url, sizeof(url), BULLETIN_SEARCH_HN_FMT, encq);
            FetchErr err = bulletin_fetch_via_reader(app, url, BULLETIN_JSON_PATH);
            if(err == FetchErrNone) {
                int before = count;
                count = bulletin_scan_source(
                    app, BULLETIN_JSON_PATH, "HN", "title", "author", "created_at", FieldIsoDate,
                    count, MAX_HEADLINES);
                // links: HN discussion page from objectID
                int nl = bulletin_scan_field(
                    BULLETIN_JSON_PATH, "objectID", FieldQuoted, app->items[0].link,
                    sizeof(Headline), LINK_LEN, before, count);
                for(int i = before; i < nl; i++) {
                    char id[16];
                    strlcpy(id, app->items[i].link, sizeof(id));
                    snprintf(
                        app->items[i].link, LINK_LEN,
                        "https://news.ycombinator.com/item?id=%s", id);
                }
            } else {
                first_err = err;
            }

            // Source 2: Reddit
            bulletin_remove_file(BULLETIN_JSON_PATH);
            snprintf(url, sizeof(url), BULLETIN_SEARCH_REDDIT_FMT, encq);
            err = bulletin_fetch_via_reader(app, url, BULLETIN_JSON_PATH);
            if(err == FetchErrNone) {
                int before = count;
                count = bulletin_scan_source(
                    app, BULLETIN_JSON_PATH, "Reddit", "title", "author", "created_utc",
                    FieldEpochDate, count, MAX_HEADLINES);
                // links: permalink -> absolute
                int nl = bulletin_scan_field(
                    BULLETIN_JSON_PATH, "permalink", FieldQuoted, app->items[0].link,
                    sizeof(Headline), LINK_LEN, before, count);
                for(int i = before; i < nl; i++) {
                    char rel[LINK_LEN];
                    strlcpy(rel, app->items[i].link, sizeof(rel));
                    strlcpy(app->items[i].link, "https://www.reddit.com", LINK_LEN);
                    strlcat(app->items[i].link, rel, LINK_LEN);
                }
            } else if(first_err == FetchErrNone) {
                first_err = err;
            }

            if(count > 0) {
                app->headline_count = count;
                app->reader_index = 0;
                ok = true;
            } else {
                app->fetch_err = (first_err != FetchErrNone) ? first_err : FetchErrParse;
            }
        } else {
            // ---- Daily Edition: HN Firebase, two stages ----
            bulletin_remove_file(BULLETIN_JSON_PATH);
            bulletin_remove_file(BULLETIN_IDS_PATH);
            memset(app->items, 0, sizeof(app->items));
            app->headline_count = 0;
            int count = 0;

            FetchErr err = bulletin_do_get(app, BULLETIN_TOPSTORIES_URL, BULLETIN_IDS_PATH, false);
            if(err == FetchErrNone) {
                uint32_t ids[BULLETIN_ITEMS];
                int nids = bulletin_parse_ids(BULLETIN_IDS_PATH, ids, BULLETIN_ITEMS);
                for(int i = 0; i < nids; i++) {
                    char url[80];
                    snprintf(url, sizeof(url), BULLETIN_ITEM_URL_FMT, (unsigned long)ids[i]);
                    if(bulletin_do_get(app, url, BULLETIN_JSON_PATH, false) != FetchErrNone) {
                        continue;
                    }
                    int before = count;
                    count = bulletin_scan_source(
                        app, BULLETIN_JSON_PATH, "HN", "title", "by", "time", FieldEpochDate,
                        count, count + 1);
                    if(count > before) {
                        // Prefer the story's external URL for details;
                        // fall back to the HN discussion page.
                        int nl = bulletin_scan_field(
                            BULLETIN_JSON_PATH, "url", FieldQuoted, app->items[0].link,
                            sizeof(Headline), LINK_LEN, before, count);
                        if(nl <= before) {
                            snprintf(
                                app->items[before].link, LINK_LEN,
                                "https://news.ycombinator.com/item?id=%lu",
                                (unsigned long)ids[i]);
                        }
                    }
                }
                if(count > 0) {
                    app->headline_count = count;
                    app->reader_index = 0;
                    ok = true;
                } else {
                    app->fetch_err = FetchErrParse;
                }
            } else {
                app->fetch_err = err;
            }
        }
    } while(false);

    view_dispatcher_send_custom_event(
        app->view_dispatcher, ok ? ok_event : BulletinEventFetchFail);
    return 0;
}

/* ------------------------------ reader view ------------------------------ */

// Shared word-wrap drawer: draws up to max_lines of text starting at
// baseline y, returns lines drawn.
static uint8_t bulletin_draw_wrapped(
    Canvas* canvas,
    const char* text,
    uint8_t y,
    uint8_t line_h,
    uint8_t max_lines) {
    char line[40];
    size_t li = 0;
    uint8_t drawn = 0;
    const char* w = text;

    while(*w && drawn < max_lines) {
        const char* start = w;
        while(*w && *w != ' ') w++;
        size_t wlen = (size_t)(w - start);
        if(wlen >= sizeof(line)) wlen = sizeof(line) - 1;

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
            canvas_draw_str(canvas, 2, y, line);
            y += line_h;
            drawn++;
            memcpy(line, start, wlen);
            line[wlen] = '\0';
            li = wlen;
        }
        while(*w == ' ') w++;
    }
    if(li > 0 && drawn < max_lines) {
        canvas_draw_str(canvas, 2, y, line);
        drawn++;
    }
    return drawn;
}

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

    const Headline* h = &app->items[app->reader_index];
    bulletin_draw_wrapped(canvas, h->title, 24, 10, 3);

    // Meta line: source / author, right-aligned date; OK hint
    canvas_draw_line(canvas, 0, 52, 127, 52);
    char meta[48];
    snprintf(meta, sizeof(meta), "%s / %s", h->source, h->author);
    size_t date_w = canvas_string_width(canvas, h->date);
    while(strlen(meta) > 1 &&
          canvas_string_width(canvas, meta) > (size_t)(124 - date_w - 6)) {
        meta[strlen(meta) - 1] = '\0';
    }
    canvas_draw_str(canvas, 2, 62, meta);
    canvas_draw_str(canvas, (uint8_t)(126 - date_w), 62, h->date);
}

static bool bulletin_reader_input(InputEvent* event, void* context);

/* ------------------------------ article view ----------------------------- */

static void bulletin_article_draw(Canvas* canvas, void* mctx) {
    ReaderModel* model = mctx;
    BulletinApp* app = model->app;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    char header[32];
    int total = app->article_line_count;
    int top = app->article_scroll;
    snprintf(header, sizeof(header), "Article");
    canvas_draw_str(canvas, 2, 10, header);
    if(total > 0) {
        char pos[16];
        int pct = ((top + ARTICLE_VISIBLE_LINES) * 100) / total;
        if(pct > 100) pct = 100;
        snprintf(pos, sizeof(pos), "%d%%", pct);
        canvas_draw_str(canvas, (uint8_t)(126 - canvas_string_width(canvas, pos)), 10, pos);
    }
    canvas_draw_line(canvas, 0, 13, 127, 13);

    canvas_set_font(canvas, FontSecondary);
    if(total == 0 || !app->article_lines) {
        canvas_draw_str(canvas, 2, 30, "No article text.");
        return;
    }
    uint8_t y = 24;
    for(int i = top; i < total && i < top + ARTICLE_VISIBLE_LINES; i++) {
        canvas_draw_str(canvas, 2, y, app->article_lines[i]);
        y += 10;
    }

    canvas_draw_line(canvas, 0, 55, 127, 55);
    if(app->photo_url[0] != '\0') {
        canvas_draw_str(canvas, 2, 63, "^v scroll        OK: photo");
    } else {
        canvas_draw_str(canvas, 2, 63, "^v scroll");
    }
}

static void bulletin_start_fetch(BulletinApp* app, FetchMode mode);

static bool bulletin_article_input(InputEvent* event, void* context) {
    BulletinApp* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool consumed = false;
    if(event->key == InputKeyDown) {
        if(app->article_scroll + ARTICLE_VISIBLE_LINES < app->article_line_count) {
            app->article_scroll++;
        }
        consumed = true;
    } else if(event->key == InputKeyUp) {
        if(app->article_scroll > 0) app->article_scroll--;
        consumed = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        if(app->photo_url[0] != '\0') {
            bulletin_start_fetch(app, FetchModePhoto);
        }
        return true;
    }

    if(consumed) {
        with_view_model(
            app->article_view, ReaderModel * model, { UNUSED(model); }, true);
    }
    return consumed;
}

/* ------------------------------- photo view ------------------------------ */

static void bulletin_photo_draw(Canvas* canvas, void* mctx) {
    ReaderModel* model = mctx;
    BulletinApp* app = model->app;

    canvas_clear(canvas);
    if(app->photo_w == 0 || app->photo_h == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 30, "No photo.");
        return;
    }
    uint8_t x = (uint8_t)((128 - app->photo_w) / 2);
    uint8_t y = (uint8_t)((64 - app->photo_h) / 2);
    canvas_draw_xbm(canvas, x, y, app->photo_w, app->photo_h, app->photo_bmp);
    canvas_draw_frame(canvas, x - 1, y - 1, app->photo_w + 2, app->photo_h + 2);
}

static bool bulletin_photo_input(InputEvent* event, void* context) {
    UNUSED(context);
    UNUSED(event);
    return false; // Back handled by previous-view callback
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

static uint32_t bulletin_nav_reader(void* context) {
    UNUSED(context);
    return BulletinViewReader;
}

static uint32_t bulletin_nav_article(void* context) {
    UNUSED(context);
    return BulletinViewArticle;
}

/* ------------------------------- callbacks ------------------------------- */

static void bulletin_show_message(BulletinApp* app, const char* title, const char* text) {
    widget_reset(app->widget);
    widget_add_string_element(app->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, title);
    widget_add_text_scroll_element(app->widget, 2, 14, 124, 50, text);
    view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewWidget);
}

static void bulletin_start_fetch(BulletinApp* app, FetchMode mode) {
    app->fetch_mode = mode;
    view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewLoading);
    furi_thread_start(app->fetch_thread);
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
    } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
        if(app->headline_count > 0 && app->items[app->reader_index].link[0] != '\0') {
            app->detail_index = app->reader_index;
            bulletin_start_fetch(app, FetchModeArticle);
        }
        return true;
    }

    if(consumed) {
        with_view_model(
            app->reader_view, ReaderModel * model, { UNUSED(model); }, true);
    }
    return consumed;
}

static void bulletin_text_input_done(void* context) {
    BulletinApp* app = context;
    switch(app->input_kind) {
    case InputSsid:
        strlcpy(app->ssid, app->input_buffer, CRED_LEN);
        bulletin_config_save(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewMenu);
        break;
    case InputPassword:
        strlcpy(app->password, app->input_buffer, CRED_LEN);
        bulletin_config_save(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewMenu);
        break;
    case InputSearch:
        strlcpy(app->query, app->input_buffer, QUERY_LEN);
        if(strlen(app->query) > 0) {
            bulletin_start_fetch(app, FetchModeSearch);
        } else {
            view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewMenu);
        }
        break;
    }
}

static void bulletin_open_text_input(
    BulletinApp* app,
    InputKind kind,
    const char* header,
    const char* preset) {
    app->input_kind = kind;
    strlcpy(app->input_buffer, preset, CRED_LEN);
    text_input_set_header_text(app->text_input, header);
    text_input_set_result_callback(
        app->text_input, bulletin_text_input_done, app, app->input_buffer, CRED_LEN, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewTextInput);
}

static void bulletin_menu_callback(void* context, uint32_t index) {
    BulletinApp* app = context;
    switch(index) {
    case BulletinMenuRead:
        bulletin_start_fetch(app, FetchModeDaily);
        break;
    case BulletinMenuSearch:
        bulletin_open_text_input(app, InputSearch, "Search News", app->query);
        break;
    case BulletinMenuSsid:
        bulletin_open_text_input(app, InputSsid, "WiFi SSID", app->ssid);
        break;
    case BulletinMenuPassword:
        bulletin_open_text_input(app, InputPassword, "WiFi Password", app->password);
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
                "Credentials saved to board.\nGive it a few seconds to\nconnect, then fetch news.");
        } else {
            bulletin_show_message(
                app, "Board Error", "Could not talk to the\nWiFi dev board over UART.");
        }
        break;
    case BulletinMenuAbout:
        bulletin_show_message(
            app,
            "Bulletin v2.2",
            "Daily edition, news search\n(HN + Reddit) with author\nand date, full-article\nreader, and in-article\nphotos dithered for the\nFlipper screen.");
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
    case BulletinEventArticleOk:
        furi_thread_join(app->fetch_thread);
        view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewArticle);
        with_view_model(
            app->article_view, ReaderModel * model, { UNUSED(model); }, true);
        return true;
    case BulletinEventPhotoOk:
        furi_thread_join(app->fetch_thread);
        view_dispatcher_switch_to_view(app->view_dispatcher, BulletinViewPhoto);
        with_view_model(
            app->photo_view, ReaderModel * model, { UNUSED(model); }, true);
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
            title = "No Results";
            hint = "Nothing usable came back.\nTry another query or story.";
            break;
        case FetchErrPhotoDecode:
            title = "Photo Failed";
            hint = "Image downloaded but\ncould not be decoded.";
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

static View* bulletin_make_view(
    BulletinApp* app,
    ViewDrawCallback draw,
    ViewInputCallback input,
    ViewNavigationCallback prev) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(ReaderModel));
    with_view_model(
        view, ReaderModel * model, { model->app = app; }, false);
    view_set_context(view, app);
    view_set_draw_callback(view, draw);
    view_set_input_callback(view, input);
    view_set_previous_callback(view, prev);
    return view;
}

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
    submenu_add_item(app->submenu, "Daily Edition", BulletinMenuRead, bulletin_menu_callback, app);
    submenu_add_item(
        app->submenu, "Search News", BulletinMenuSearch, bulletin_menu_callback, app);
    submenu_add_item(app->submenu, "Set WiFi SSID", BulletinMenuSsid, bulletin_menu_callback, app);
    submenu_add_item(
        app->submenu, "Set WiFi Password", BulletinMenuPassword, bulletin_menu_callback, app);
    submenu_add_item(
        app->submenu, "Send WiFi to Board", BulletinMenuConnect, bulletin_menu_callback, app);
    submenu_add_item(app->submenu, "About", BulletinMenuAbout, bulletin_menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), bulletin_nav_exit);
    view_dispatcher_add_view(
        app->view_dispatcher, BulletinViewMenu, submenu_get_view(app->submenu));

    // Text input (shared: SSID / password / search query)
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

    // Reader / Article / Photo
    app->reader_view =
        bulletin_make_view(app, bulletin_reader_draw, bulletin_reader_input, bulletin_nav_menu);
    view_dispatcher_add_view(app->view_dispatcher, BulletinViewReader, app->reader_view);

    app->article_view = bulletin_make_view(
        app, bulletin_article_draw, bulletin_article_input, bulletin_nav_reader);
    view_dispatcher_add_view(app->view_dispatcher, BulletinViewArticle, app->article_view);

    app->photo_view =
        bulletin_make_view(app, bulletin_photo_draw, bulletin_photo_input, bulletin_nav_article);
    view_dispatcher_add_view(app->view_dispatcher, BulletinViewPhoto, app->photo_view);

    // Fetch worker thread (started on demand). Generous stack: it calls
    // into storage + FlipperHTTP, parses files, and decodes JPEGs.
    app->fetch_thread =
        furi_thread_alloc_ex("BulletinFetch", 8192, bulletin_fetch_worker, app);

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
    view_dispatcher_remove_view(app->view_dispatcher, BulletinViewArticle);
    view_free(app->article_view);
    view_dispatcher_remove_view(app->view_dispatcher, BulletinViewPhoto);
    view_free(app->photo_view);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    if(app->article_lines) free(app->article_lines);
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
