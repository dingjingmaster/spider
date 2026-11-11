#include "spiders/archive_org/private/filter.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static size_t utf8_decode(const unsigned char *p, uint32_t *cp)
{
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if (p[0] >= 0xC2 && p[0] <= 0xDF && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if (p[0] >= 0xE0 && p[0] <= 0xEF && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if (p[0] >= 0xF0 && p[0] <= 0xF4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = p[0];
    return 1;
}

static bool cjk_codepoint(uint32_t cp)
{
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF);
}

static bool space_codepoint(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r';
}

double archive_cjk_ratio(const char *text)
{
    if (!text)
        return 0;
    size_t chars = 0, cjk = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t cp = 0;
        p += utf8_decode(p, &cp);
        if (!space_codepoint(cp))
            chars++;
        if (cjk_codepoint(cp))
            cjk++;
    }
    return chars ? (double)cjk / (double)chars : 0;
}

bool archive_has_cjk(const char *text)
{
    if (!text)
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t cp = 0;
        p += utf8_decode(p, &cp);
        if (cjk_codepoint(cp))
            return true;
    }
    return false;
}

bool archive_has_non_ascii(const char *text)
{
    if (!text)
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t cp = 0;
        p += utf8_decode(p, &cp);
        if (cp > 0x7F)
            return true;
    }
    return false;
}

bool archive_text_contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !*needle)
        return false;
    for (const char *p = text; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b)
            return true;
    }
    return false;
}

static bool ends_with_ci(const char *text, const char *suffix)
{
    if (!text || !suffix)
        return false;
    size_t text_len = strlen(text), suffix_len = strlen(suffix);
    return text_len >= suffix_len && !strcasecmp(text + text_len - suffix_len, suffix);
}

bool archive_is_audio(const char *format, const char *name)
{
    if (name) {
        static const char *ignored[] = {".png", ".jpg", ".jpeg", ".gif", ".torrent",
                                        ".xml", ".sqlite", ".txt", ".pdf"};
        for (size_t i = 0; i < sizeof(ignored) / sizeof(*ignored); i++)
            if (ends_with_ci(name, ignored[i]))
                return false;
        if (strstr(name, "_spectrogram") || strstr(name, "_files.") || strstr(name, "_meta."))
            return false;
    }
    if (format) {
        static const char *formats[] = {"MP3", "FLAC", "OGG", "WAVE", "M4A",
                                        "AAC", "ALAC", "APPLE LOSSLESS"};
        for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); i++)
            if (archive_text_contains_ci(format, formats[i]))
                return true;
    }
    if (name) {
        static const char *extensions[] = {".mp3", ".flac", ".ogg", ".oga", ".m4a",
                                           ".wav", ".aac", ".opus"};
        for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++)
            if (ends_with_ci(name, extensions[i]))
                return true;
    }
    return false;
}

int archive_format_rank(const char *format, json_object *prefer)
{
    static const char *defaults[] = {"24bit Flac", "Flac", "320Kbps MP3", "VBR MP3",
                                     "128Kbps MP3", "MP3", "Ogg Vorbis", "WAVE",
                                     "Apple Lossless"};
    if (!format)
        return 100;
    if (prefer && json_object_is_type(prefer, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(prefer); i++) {
            json_object *value = json_object_array_get_idx(prefer, i);
            if (!json_object_is_type(value, json_type_string))
                continue;
            const char *candidate = json_object_get_string(value);
            if (archive_text_contains_ci(format, candidate))
                return (int)i;
        }
        return 100;
    }
    for (size_t i = 0; i < sizeof(defaults) / sizeof(*defaults); i++)
        if (archive_text_contains_ci(format, defaults[i]))
            return (int)i;
    return 100;
}

double archive_parse_length(const char *length)
{
    if (!length || !*length)
        return 0;
    if (strchr(length, ':')) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%s", length);
        double total = 0;
        for (char *token = strtok(buffer, ":"); token; token = strtok(NULL, ":"))
            total = total * 60 + strtod(token, NULL);
        return total;
    }
    return strtod(length, NULL);
}

bool archive_list_contains(json_object *list, const char *needle)
{
    if (!list)
        return false;
    if (json_object_is_type(list, json_type_string))
        return archive_text_contains_ci(json_object_get_string(list), needle);
    if (json_object_is_type(list, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(list); i++) {
            json_object *value = json_object_array_get_idx(list, i);
            if (json_object_is_type(value, json_type_string) &&
                archive_text_contains_ci(json_object_get_string(value), needle))
                return true;
        }
    }
    return false;
}

static const char *base_name(const char *name)
{
    const char *slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

static void without_extension(const char *name, char *out, size_t cap)
{
    size_t len = strlen(name);
    const char *slash = strrchr(name, '/');
    const char *dot = strrchr(name, '.');
    if (dot && dot != name && (!slash || dot > slash))
        len = (size_t)(dot - name);
    if (len >= cap)
        len = cap - 1;
    memcpy(out, name, len);
    out[len] = 0;
}

static const char *separator(const char *name)
{
    static const char *separators[] = {" - ", " – ", " — ", " _ "};
    for (size_t i = 0; i < sizeof(separators) / sizeof(*separators); i++) {
        const char *found = strstr(name, separators[i]);
        if (found)
            return found;
    }
    return NULL;
}

static void trim(char *text)
{
    char *start = text;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (start != text)
        memmove(text, start, strlen(start) + 1);
    size_t len = strlen(text);
    while (len && isspace((unsigned char)text[len - 1]))
        text[--len] = 0;
}

void archive_artist_title(const char *file_name, const char *file_artist,
                          const char *file_title, const char *item_creator, char *artist,
                          size_t artist_cap, char *title, size_t title_cap)
{
    artist[0] = 0;
    title[0] = 0;
    const char *base = base_name(file_name ? file_name : "");
    const char *sep = separator(base);
    if (file_artist && *file_artist)
        snprintf(artist, artist_cap, "%s", file_artist);
    else if (sep) {
        size_t len = (size_t)(sep - base);
        if (len >= artist_cap)
            len = artist_cap - 1;
        memcpy(artist, base, len);
        artist[len] = 0;
        trim(artist);
    } else if (item_creator)
        snprintf(artist, artist_cap, "%s", item_creator);
    if (file_title && *file_title)
        snprintf(title, title_cap, "%s", file_title);
    else if (sep) {
        snprintf(title, title_cap, "%s", sep + 3);
        char *dot = strrchr(title, '.');
        if (dot && dot != title)
            *dot = 0;
        trim(title);
    } else
        without_extension(base, title, title_cap);
}

size_t archive_file_size(json_object *file)
{
    json_object *value = jg(file, "size");
    if (!value)
        return 0;
    if (json_object_is_type(value, json_type_int))
        return (size_t)json_object_get_int64(value);
    if (json_object_is_type(value, json_type_double))
        return (size_t)json_object_get_double(value);
    if (json_object_is_type(value, json_type_string))
        return (size_t)strtoull(json_object_get_string(value), NULL, 10);
    return 0;
}

double archive_file_seconds(json_object *file)
{
    double seconds = archive_parse_length(js(file, "length", ""));
    if (seconds > 0)
        return seconds;
    json_object *value = jg(file, "seconds");
    if (value && (json_object_is_type(value, json_type_double) ||
                  json_object_is_type(value, json_type_int)))
        return json_object_get_double(value);
    return 0;
}

int archive_select_files(json_object *metadata, json_object *config, json_object **selected,
                         char *error, size_t cap)
{
    (void)error;
    (void)cap;
    *selected = json_object_new_array();
    json_object *files = jg(metadata, "files");
    if (!files || !json_object_is_type(files, json_type_array))
        return 0;
    json_object *prefer = jg(config, "format_prefer");
    typedef struct {
        char key[512];
        int rank;
        json_object *file;
    } Selection;
    Selection *groups = NULL;
    size_t count = 0, capacity = 0;
    for (size_t i = 0; i < json_object_array_length(files); i++) {
        json_object *file = json_object_array_get_idx(files, i);
        const char *name = js(file, "name", "");
        const char *format = js(file, "format", "");
        if (!*name || !archive_is_audio(format, name))
            continue;
        char key[512];
        const char *original = js(file, "original", "");
        if (*original)
            snprintf(key, sizeof(key), "%s", original);
        else
            without_extension(name, key, sizeof(key));
        if (!*key)
            snprintf(key, sizeof(key), "%s", name);
        int rank = archive_format_rank(format, prefer);
        size_t found = count;
        for (size_t j = 0; j < count; j++)
            if (!strcmp(groups[j].key, key)) {
                found = j;
                break;
            }
        if (found == count) {
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 8;
                Selection *grown = realloc(groups, next * sizeof(*groups));
                if (!grown) {
                    for (size_t j = 0; j < count; j++)
                        json_object_put(groups[j].file);
                    free(groups);
                    return fail(error, cap, "Cannot allocate file selection");
                }
                groups = grown;
                capacity = next;
            }
            snprintf(groups[count].key, sizeof(groups[count].key), "%s", key);
            groups[count].rank = rank;
            groups[count].file = json_object_get(file);
            count++;
        } else if (rank < groups[found].rank) {
            json_object_put(groups[found].file);
            groups[found].file = json_object_get(file);
            groups[found].rank = rank;
        }
    }
    for (size_t i = 0; i < count; i++) {
        json_object *entry = json_object_get(groups[i].file);
        json_object_object_add(entry, "__key", json_object_new_string(groups[i].key));
        json_object_array_add(*selected, entry);
        json_object_put(groups[i].file);
    }
    free(groups);
    return 0;
}

static void first_string(json_object *value, char *buffer, size_t cap)
{
    buffer[0] = 0;
    if (!value)
        return;
    if (json_object_is_type(value, json_type_string)) {
        snprintf(buffer, cap, "%s", json_object_get_string(value));
        return;
    }
    if (!json_object_is_type(value, json_type_array))
        return;
    size_t used = 0;
    for (size_t i = 0; i < json_object_array_length(value); i++) {
        json_object *item = json_object_array_get_idx(value, i);
        if (!json_object_is_type(item, json_type_string))
            continue;
        int written = snprintf(buffer + used, cap - used, "%s%s", used ? "; " : "",
                               json_object_get_string(item));
        if (written < 0 || (size_t)written >= cap - used)
            break;
        used += (size_t)written;
    }
}

static bool list_hit(json_object *list, const char *needle)
{
    if (!list || !json_object_is_type(list, json_type_array))
        return false;
    for (size_t i = 0; i < json_object_array_length(list); i++) {
        json_object *value = json_object_array_get_idx(list, i);
        if (json_object_is_type(value, json_type_string) &&
            archive_text_contains_ci(needle, json_object_get_string(value)))
            return true;
    }
    return false;
}

static bool language_chinese(const char *language)
{
    static const char *markers[] = {"chi", "zho", "cmn", "yue", "chinese", "中文"};
    for (size_t i = 0; i < sizeof(markers) / sizeof(*markers); i++)
        if (archive_text_contains_ci(language, markers[i]))
            return true;
    return false;
}

static bool music_subject(json_object *subject)
{
    static const char *markers[] = {"mandopop", "cantopop", "cpop", "chinese pop",
                                    "chinese music", "chinese classical", "华语", "中文",
                                    "国语", "粤语", "民歌", "music of china", "china audio"};
    for (size_t i = 0; i < sizeof(markers) / sizeof(*markers); i++)
        if (archive_list_contains(subject, markers[i]))
            return true;
    return false;
}

static void reason(ArchiveDecision *decision, const char *fmt, ...)
{
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    json_object_array_add(decision->reasons, json_object_new_string(text));
}

static bool reject(ArchiveDecision *decision, const char *fmt, ...)
{
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    json_object_array_add(decision->reasons, json_object_new_string(text));
    decision->include = false;
    return false;
}

int archive_judge(json_object *metadata, json_object *file, json_object *config,
                  size_t audio_count, ArchiveDecision *decision, char *error, size_t cap)
{
    (void)error;
    (void)cap;
    decision->include = false;
    decision->score = 0;
    decision->reasons = json_object_new_array();
    json_object *item = jg(metadata, "metadata");
    json_object *subject = jg(item, "subject");
    json_object *collection = jg(item, "collection");
    const char *item_title = js(item, "title", "");
    const char *item_language = js(item, "language", "");
    const char *item_description = js(item, "description", "");
    char item_creator[512];
    first_string(jg(item, "creator"), item_creator, sizeof(item_creator));

    json_object *exclude_collections = jg(config, "exclude_collections");
    if (exclude_collections && json_object_is_type(exclude_collections, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(exclude_collections); i++) {
            json_object *value = json_object_array_get_idx(exclude_collections, i);
            if (!json_object_is_type(value, json_type_string))
                continue;
            const char *excluded = json_object_get_string(value);
            if (archive_list_contains(collection, excluded)) {
                reject(decision, "excluded collection: %s", excluded);
                return 0;
            }
        }
    }
    json_object *exclude_keywords = jg(config, "exclude_keywords");
    const char *file_name = js(file, "name", "");
    const char *file_title = js(file, "title", "");
    const char *checks[] = {item_title, item_creator, item_description, file_name, file_title};
    for (size_t i = 0; i < sizeof(checks) / sizeof(*checks); i++) {
        if (!exclude_keywords || !json_object_is_type(exclude_keywords, json_type_array))
            break;
        for (size_t j = 0; j < json_object_array_length(exclude_keywords); j++) {
            json_object *value = json_object_array_get_idx(exclude_keywords, j);
            if (json_object_is_type(value, json_type_string) &&
                archive_text_contains_ci(checks[i], json_object_get_string(value))) {
                reject(decision, "excluded keyword: %s", json_object_get_string(value));
                return 0;
            }
        }
    }
    if (exclude_keywords && json_object_is_type(exclude_keywords, json_type_array)) {
        for (size_t j = 0; j < json_object_array_length(exclude_keywords); j++) {
            json_object *value = json_object_array_get_idx(exclude_keywords, j);
            if (json_object_is_type(value, json_type_string) &&
                archive_list_contains(subject, json_object_get_string(value))) {
                reject(decision, "excluded subject: %s", json_object_get_string(value));
                return 0;
            }
        }
    }

    double seconds = archive_file_seconds(file);
    int min_seconds = ji(config, "min_seconds", 30);
    int max_seconds = ji(config, "max_seconds", 1200);
    if (seconds > 0 && (seconds < min_seconds || seconds > max_seconds)) {
        reject(decision, "duration %.0fs out of range", seconds);
        return 0;
    }
    size_t bytes = archive_file_size(file);
    size_t min_bytes = (size_t)ji(config, "min_bytes", 20480);
    if (bytes && bytes < min_bytes) {
        reject(decision, "size %zu below minimum", bytes);
        return 0;
    }

    char artist[256], title[512];
    archive_artist_title(file_name, js(file, "artist", ""), file_title, item_creator, artist,
                         sizeof(artist), title, sizeof(title));
    json_object *whitelist = jg(config, "artist_whitelist");
    bool whitelist_hit = list_hit(whitelist, item_creator) || list_hit(whitelist, item_title) ||
                         list_hit(whitelist, file_name) || list_hit(whitelist, artist);
    if (jb(config, "artist_whitelist_strict", false) && !whitelist_hit) {
        reject(decision, "artist not in whitelist");
        return 0;
    }

    double item_cjk =
        fmax(archive_cjk_ratio(item_title), archive_cjk_ratio(item_creator));
    double file_cjk = archive_cjk_ratio(file_name);
    double creator_cjk = archive_cjk_ratio(item_creator);
    bool file_evidence = archive_has_cjk(file_name) || archive_has_cjk(artist) ||
                         archive_has_cjk(file_title);
    bool non_ascii = archive_has_non_ascii(file_name);
    if (audio_count > 100 && !file_evidence && !whitelist_hit) {
        reject(decision, "large compilation without Chinese filename");
        return 0;
    }

    if (music_subject(subject)) {
        decision->score += 3;
        reason(decision, "music subject");
    }
    if (language_chinese(item_language)) {
        decision->score += 2;
        reason(decision, "Chinese language");
    }
    if (item_cjk >= 0.3) {
        decision->score += 2;
        reason(decision, "item title CJK");
    }
    if (file_cjk >= 0.3) {
        decision->score += 2;
        reason(decision, "filename CJK");
    }
    if (creator_cjk >= 0.3) {
        decision->score += 1;
        reason(decision, "creator CJK");
    }
    if (strstr(file_name, " - ")) {
        decision->score += 1;
        reason(decision, "artist-title filename");
    }
    if (whitelist_hit) {
        decision->score += 4;
        reason(decision, "artist whitelist");
    }
    if (non_ascii && !file_evidence) {
        decision->score -= 2;
        reason(decision, "non-Chinese filename");
    }
    if (audio_count > 100 && !file_evidence) {
        decision->score -= 5;
        reason(decision, "large compilation");
    }
    int min_score = ji(config, "min_score", 3);
    decision->include = jb(config, "download_all", true) || decision->score >= min_score;
    return 0;
}
