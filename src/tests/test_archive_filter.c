/* Archive.org filter/selection helpers. */
#include "spiders/archive_org/private/filter.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_object *parse(const char *raw)
{
    json_object *o = json_tokener_parse(raw);
    assert(o);
    return o;
}

static bool valid_utf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        size_t length;
        uint32_t cp;
        if (*p < 0x80) {
            p++;
            continue;
        } else if (*p >= 0xC2 && *p <= 0xDF) {
            length = 2;
            cp = *p & 0x1F;
        } else if (*p >= 0xE0 && *p <= 0xEF) {
            length = 3;
            cp = *p & 0x0F;
        } else if (*p >= 0xF0 && *p <= 0xF4) {
            length = 4;
            cp = *p & 0x07;
        } else {
            return false;
        }
        for (size_t i = 1; i < length; i++) {
            if ((p[i] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if ((length == 3 && cp < 0x800) || (length == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        p += length;
    }
    return true;
}

static json_object *config(void)
{
    return parse("{\"artist_whitelist\":[],\"artist_whitelist_strict\":false,"
                 "\"exclude_keywords\":[\"有声书\",\"评书\",\"相声\",\"电台\"],"
                 "\"exclude_collections\":[\"radio\",\"podcasts\"],"
                 "\"format_prefer\":[\"24bit Flac\",\"Flac\",\"320Kbps MP3\",\"VBR MP3\","
                 "\"128Kbps MP3\",\"MP3\",\"Ogg Vorbis\"],"
                 "\"min_score\":3,\"min_seconds\":30,\"max_seconds\":1200,\"min_bytes\":20480}");
}

static json_object *metadata(void)
{
    return parse(
        "{\"metadata\":{\"identifier\":\"item1\",\"title\":\"Music of China - Vol. 8 - 邓丽君 "
        "我只在乎你\",\"creator\":\"RAN.DUMP\",\"subject\":[\"Chinese Music\",\"Music of "
        "China\"],\"collection\":[\"opensource_audio\"],\"language\":\"chi\",\"description\":\"华语"
        "音乐\",\"server\":\"ia.example\",\"dir\":\"/0/items/item1\"},"
        "\"files\":["
        "{\"name\":\"赵雷 - 成都.mp3\",\"format\":\"VBR MP3\",\"size\":\"5200000\",\"length\":\"5:30\","
        "\"sha1\":\"mp3sha\",\"original\":\"赵雷 - 成都.flac\"},"
        "{\"name\":\"赵雷 - 成都.flac\",\"format\":\"24bit Flac\",\"size\":\"50000000\","
        "\"length\":\"5:30\",\"sha1\":\"flacsha\",\"original\":\"赵雷 - 成都.flac\"},"
        "{\"name\":\"เพลงไทย.mp3\",\"format\":\"VBR MP3\",\"size\":\"4000000\",\"length\":\"4:00\","
        "\"sha1\":\"thaisha\"},"
        "{\"name\":\"cover.png\",\"format\":\"PNG\",\"size\":\"100\"}]}");
}

static json_object *file_by_name(json_object *root, const char *name)
{
    json_object *files = jg(root, "files");
    for (size_t i = 0; i < json_object_array_length(files); i++) {
        json_object *file = json_object_array_get_idx(files, i);
        if (!strcmp(js(file, "name", ""), name))
            return file;
    }
    assert(0 && "fixture file not found");
    return NULL;
}

static void test_cjk(void)
{
    assert(archive_cjk_ratio("赵雷 - 成都") > 0.5);
    assert(archive_cjk_ratio("hello") == 0);
    assert(archive_has_cjk("赵雷"));
    assert(!archive_has_cjk("hello"));
    assert(archive_has_non_ascii("เพลงไทย"));
    assert(!archive_has_non_ascii("hello"));
    assert(archive_text_contains_ci("Chinese Music", "chinese"));
    json_object *list = parse("[\"Chinese Music\",\"mandopop\"]");
    assert(archive_list_contains(list, "mandopop"));
    assert(!archive_list_contains(list, "radio"));
    json_object_put(list);
}

static void test_audio_and_length(void)
{
    assert(archive_is_audio("VBR MP3", "a.mp3"));
    assert(archive_is_audio("24bit Flac", "a.flac"));
    assert(!archive_is_audio("PNG", "a.png"));
    assert(!archive_is_audio("Spectrogram", "a_spectrogram.png"));
    json_object *prefer =
        parse("[\"24bit Flac\",\"Flac\",\"320Kbps MP3\",\"VBR MP3\",\"128Kbps MP3\",\"MP3\"]");
    assert(archive_format_rank("24bit Flac", prefer) < archive_format_rank("VBR MP3", prefer));
    assert(archive_format_rank("Flac", prefer) < archive_format_rank("128Kbps MP3", prefer));
    json_object_put(prefer);
    assert(fabs(archive_parse_length("5:30") - 330) < 0.01);
    assert(fabs(archive_parse_length("1:02:03") - 3723) < 0.01);
    assert(fabs(archive_parse_length("330.5") - 330.5) < 0.01);
    assert(archive_parse_length("") == 0);
}

static void test_artist_title(void)
{
    char artist[256], title[512];
    archive_artist_title("赵雷 - 成都.mp3", NULL, NULL, "RAN.DUMP", artist, sizeof(artist), title,
                         sizeof(title));
    assert(!strcmp(artist, "赵雷"));
    assert(!strcmp(title, "成都"));
    archive_artist_title("01.mp3", NULL, NULL, "邓丽君", artist, sizeof(artist), title,
                         sizeof(title));
    assert(!strcmp(artist, "邓丽君"));
    assert(!strcmp(title, "01"));
    archive_artist_title("song.mp3", "Artist", "Title", NULL, artist, sizeof(artist), title,
                         sizeof(title));
    assert(!strcmp(artist, "Artist"));
    assert(!strcmp(title, "Title"));
}

static void test_artist_truncation_keeps_utf8(void)
{
    char creator[601];
    size_t used = 0;
    while (used + 2 < sizeof(creator)) {
        memcpy(creator + used, "é", 2);
        used += 2;
    }
    creator[used] = 0;
    char artist[256], title[512];
    archive_artist_title("song.mp3", NULL, NULL, creator, artist, sizeof(artist), title,
                         sizeof(title));
    assert(valid_utf8(artist));
}

static void test_select_files(void)
{
    json_object *root = metadata();
    json_object *cfg = config();
    json_object *selected = NULL;
    char error[SP_ERROR] = {0};
    assert(archive_select_files(root, cfg, &selected, error, sizeof(error)) == 0);
    assert(json_object_is_type(selected, json_type_array));
    assert(json_object_array_length(selected) == 2);
    bool flac = false, thai = false;
    for (size_t i = 0; i < json_object_array_length(selected); i++) {
        json_object *entry = json_object_array_get_idx(selected, i);
        const char *key = js(entry, "__key", "");
        if (!strcmp(key, "赵雷 - 成都.flac")) {
            flac = true;
            assert(!strcmp(js(entry, "format", ""), "24bit Flac"));
        }
        if (strstr(key, "เพลงไทย"))
            thai = true;
    }
    assert(flac && thai);
    json_object_put(selected);
    json_object_put(cfg);
    json_object_put(root);
}

static void test_judge_chinese_and_thai(void)
{
    json_object *root = metadata(), *cfg = config();
    ArchiveDecision chinese = {0}, thai = {0};
    char error[SP_ERROR] = {0};
    assert(archive_judge(root, file_by_name(root, "赵雷 - 成都.mp3"), cfg, 500, &chinese, error,
                         sizeof(error)) == 0);
    assert(chinese.include);
    assert(chinese.score >= 3);
    assert(chinese.reasons && json_object_array_length(chinese.reasons) > 0);
    assert(archive_judge(root, file_by_name(root, "เพลงไทย.mp3"), cfg, 500, &thai, error,
                         sizeof(error)) == 0);
    assert(!thai.include);
    json_object_put(chinese.reasons);
    json_object_put(thai.reasons);
    json_object_put(cfg);
    json_object_put(root);
}

static void test_judge_whitelist(void)
{
    json_object *root = parse(
        "{\"metadata\":{\"identifier\":\"item2\",\"title\":\"Compilation\",\"creator\":\"Various\","
        "\"subject\":[\"Chinese Music\"],\"collection\":[\"opensource_audio\"],\"language\":\"chi\"},"
        "\"files\":[{\"name\":\"chengdu.mp3\",\"format\":\"VBR MP3\",\"size\":\"5200000\","
        "\"length\":\"5:30\",\"artist\":\"赵雷\",\"title\":\"成都\"}]}");
    json_object *cfg = config();
    json_object_object_add(cfg, "artist_whitelist", parse("[\"赵雷\"]"));
    ArchiveDecision decision = {0};
    char error[SP_ERROR] = {0};
    assert(archive_judge(root, file_by_name(root, "chengdu.mp3"), cfg, 500, &decision, error,
                         sizeof(error)) == 0);
    assert(decision.include);
    json_object_put(decision.reasons);
    json_object_object_add(cfg, "artist_whitelist_strict", json_object_new_boolean(true));
    ArchiveDecision strict = {0};
    assert(archive_judge(root, file_by_name(root, "chengdu.mp3"), cfg, 500, &strict, error,
                         sizeof(error)) == 0);
    assert(strict.include);
    json_object_put(strict.reasons);
    json_object_put(cfg);
    json_object_put(root);
}

static void test_judge_exclusions(void)
{
    json_object *root = parse(
        "{\"metadata\":{\"identifier\":\"book\",\"title\":\"有声书 三国演义\",\"creator\":\"主播\","
        "\"subject\":[\"audiobook\"],\"collection\":[\"radio\"],\"language\":\"chi\"},"
        "\"files\":[{\"name\":\"赵雷 - 成都.mp3\",\"format\":\"VBR MP3\",\"size\":\"5200000\","
        "\"length\":\"5:30\"}]}");
    json_object *cfg = config();
    ArchiveDecision decision = {0};
    char error[SP_ERROR] = {0};
    assert(archive_judge(root, file_by_name(root, "赵雷 - 成都.mp3"), cfg, 10, &decision, error,
                         sizeof(error)) == 0);
    assert(!decision.include);
    json_object_put(decision.reasons);
    json_object_put(cfg);
    json_object_put(root);
}

static void test_judge_duration_and_size(void)
{
    json_object *root = parse(
        "{\"metadata\":{\"identifier\":\"item3\",\"title\":\"华语歌曲\",\"creator\":\"歌手\","
        "\"subject\":[\"Chinese Music\"],\"collection\":[\"opensource_audio\"],\"language\":\"chi\"},"
        "\"files\":["
        "{\"name\":\"short.mp3\",\"format\":\"VBR MP3\",\"size\":\"5200000\",\"length\":\"0:10\"},"
        "{\"name\":\"long.mp3\",\"format\":\"VBR MP3\",\"size\":\"5200000\",\"length\":\"30:00\"},"
        "{\"name\":\"tiny.mp3\",\"format\":\"VBR MP3\",\"size\":\"1000\",\"length\":\"5:30\"}]}");
    json_object *cfg = config();
    char error[SP_ERROR] = {0};
    const char *names[] = {"short.mp3", "long.mp3", "tiny.mp3"};
    for (size_t i = 0; i < 3; i++) {
        ArchiveDecision decision = {0};
        assert(archive_judge(root, file_by_name(root, names[i]), cfg, 10, &decision, error,
                             sizeof(error)) == 0);
        assert(!decision.include);
        json_object_put(decision.reasons);
    }
    json_object_put(cfg);
    json_object_put(root);
}

static void test_judge_subject_exclusion(void)
{
    json_object *root = parse(
        "{\"metadata\":{\"identifier\":\"podcast\",\"title\":\"Music\",\"creator\":\"歌手\","
        "\"subject\":[\"Podcast\"],\"collection\":[\"opensource_audio\"],\"language\":\"chi\"},"
        "\"files\":[{\"name\":\"赵雷 - 成都.mp3\",\"format\":\"VBR MP3\",\"size\":\"5200000\","
        "\"length\":\"5:30\"}]}");
    json_object *cfg = config();
    json_object_object_add(cfg, "exclude_keywords", parse("[\"podcast\"]"));
    ArchiveDecision decision = {0};
    char error[SP_ERROR] = {0};
    assert(archive_judge(root, file_by_name(root, "赵雷 - 成都.mp3"), cfg, 10, &decision, error,
                         sizeof(error)) == 0);
    assert(!decision.include);
    json_object_put(decision.reasons);
    json_object_put(cfg);
    json_object_put(root);
}

static void test_judge_download_all(void)
{
    json_object *root = parse(
        "{\"metadata\":{\"identifier\":\"low\",\"title\":\"x\",\"creator\":\"y\","
        "\"collection\":[\"opensource_audio\"]},\"files\":[{\"name\":\"เพลง.mp3\","
        "\"format\":\"VBR MP3\",\"size\":\"5200000\",\"length\":\"5:30\"}]}");
    json_object *cfg = config();
    json_object_object_add(cfg, "min_score", json_object_new_int(100));
    char error[SP_ERROR] = {0};
    ArchiveDecision decision = {0};
    assert(archive_judge(root, file_by_name(root, "เพลง.mp3"), cfg, 10, &decision, error,
                         sizeof(error)) == 0);
    assert(decision.include);
    json_object_put(decision.reasons);
    json_object_object_add(cfg, "download_all", json_object_new_boolean(false));
    ArchiveDecision gated = {0};
    assert(archive_judge(root, file_by_name(root, "เพลง.mp3"), cfg, 10, &gated, error,
                         sizeof(error)) == 0);
    assert(!gated.include);
    json_object_put(gated.reasons);
    json_object_put(cfg);
    json_object_put(root);
}

int main(void)
{
    test_cjk();
    test_audio_and_length();
    test_artist_title();
    test_artist_truncation_keeps_utf8();
    test_select_files();
    test_judge_chinese_and_thai();
    test_judge_whitelist();
    test_judge_exclusions();
    test_judge_subject_exclusion();
    test_judge_download_all();
    test_judge_duration_and_size();
    puts("archive.org filter passed");
    return 0;
}
