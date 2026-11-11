#ifndef SPIDER_ARCHIVE_ORG_FILTER_H
#define SPIDER_ARCHIVE_ORG_FILTER_H
#include "common/common.h"

typedef struct {
    bool include;
    int score;
    json_object *reasons;   /* owned array of strings */
} ArchiveDecision;

/* Pure helpers shared by the spider and its tests. */
double archive_cjk_ratio(const char *text);
bool archive_has_cjk(const char *text);
bool archive_has_non_ascii(const char *text);
bool archive_is_audio(const char *format, const char *name);
int archive_format_rank(const char *format, json_object *prefer);
double archive_parse_length(const char *length);
size_t archive_file_size(json_object *file);
double archive_file_seconds(json_object *file);
bool archive_text_contains_ci(const char *text, const char *needle);
bool archive_list_contains(json_object *list, const char *needle);
void archive_artist_title(const char *file_name, const char *file_artist,
                          const char *file_title, const char *item_creator, char *artist,
                          size_t artist_cap, char *title, size_t title_cap);

/* Select one preferred audio file per logical track. Each returned entry is the
 * original file object with an added "__key" field. */
int archive_select_files(json_object *metadata, json_object *config, json_object **selected,
                         char *error, size_t cap);
int archive_judge(json_object *metadata, json_object *file, json_object *config,
                  size_t audio_count, ArchiveDecision *decision, char *error, size_t cap);
#endif
