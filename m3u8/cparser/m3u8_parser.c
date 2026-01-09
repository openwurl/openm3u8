/*
 * m3u8_parser.c - Pure C parser for HLS M3U8 manifests
 *
 * This implementation parses M3U8 playlists entirely in C, avoiding
 * Python object allocation during the hot loop for maximum performance.
 */

#include "m3u8_parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ============================================================================
 * Memory allocation helpers
 * ========================================================================= */

static char* str_dup(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = (char*)malloc(len + 1);
    if (dst) {
        memcpy(dst, src, len + 1);
    }
    return dst;
}

static char* str_ndup(const char *src, size_t n) {
    if (!src) return NULL;
    char *dst = (char*)malloc(n + 1);
    if (dst) {
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return dst;
}

/* ============================================================================
 * String utilities
 * ========================================================================= */

/* Skip leading whitespace */
static const char* skip_ws(const char *s) {
    while (*s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

/* Find end of line, returns pointer to '\n' or end of string */
static const char* find_eol(const char *s, const char *end) {
    while (s < end && *s != '\n' && *s != '\r') s++;
    return s;
}

/* Check if line starts with prefix */
static int starts_with(const char *line, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    if (len < plen) return 0;
    return memcmp(line, prefix, plen) == 0;
}

/* Remove surrounding quotes from a string */
static char* remove_quotes(const char *s, size_t len) {
    if (len >= 2) {
        if ((s[0] == '"' && s[len-1] == '"') ||
            (s[0] == '\'' && s[len-1] == '\'')) {
            return str_ndup(s + 1, len - 2);
        }
    }
    return str_ndup(s, len);
}

/* Normalize attribute name: replace '-' with '_' and lowercase */
static void normalize_attr_name(char *name) {
    for (char *p = name; *p; p++) {
        if (*p == '-') *p = '_';
        else *p = (char)tolower((unsigned char)*p);
    }
    /* Trim trailing whitespace */
    size_t len = strlen(name);
    while (len > 0 && (name[len-1] == ' ' || name[len-1] == '\t')) {
        name[--len] = '\0';
    }
}

/* ============================================================================
 * Attribute parsing (replaces regex-based ATTRIBUTELISTPATTERN)
 * ========================================================================= */

/*
 * Parse attribute list like: KEY1=value1,KEY2="value2",KEY3='value3'
 * Returns linked list of M3U8Attribute
 */
static M3U8Attribute* parse_attributes(const char *line, size_t len) {
    M3U8Attribute *head = NULL;
    M3U8Attribute *tail = NULL;
    
    const char *p = line;
    const char *end = line + len;
    
    while (p < end) {
        /* Skip whitespace */
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        
        /* Find '=' or end of token */
        const char *eq = p;
        while (eq < end && *eq != '=' && *eq != ',') eq++;
        
        if (eq >= end || *eq == ',') {
            /* No '=' found, this is a bare value */
            const char *val_start = p;
            const char *val_end = eq;
            
            /* Trim whitespace */
            while (val_end > val_start && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
            
            if (val_end > val_start) {
                M3U8Attribute *attr = (M3U8Attribute*)calloc(1, sizeof(M3U8Attribute));
                if (attr) {
                    attr->key = str_dup("");
                    attr->value = str_ndup(val_start, val_end - val_start);
                    if (tail) {
                        tail->next = attr;
                        tail = attr;
                    } else {
                        head = tail = attr;
                    }
                }
            }
            
            if (*eq == ',') p = eq + 1;
            else p = eq;
            continue;
        }
        
        /* Extract key */
        const char *key_start = p;
        const char *key_end = eq;
        while (key_end > key_start && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
        
        /* Move past '=' */
        p = eq + 1;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        
        /* Extract value (handle quoted strings, but PRESERVE quotes) */
        const char *val_start = p;
        const char *val_end;
        
        if (p < end && (*p == '"' || *p == '\'')) {
            char quote = *p++;
            /* Skip past the content */
            while (p < end && *p != quote) p++;
            if (p < end) p++;  /* Skip closing quote */
            val_end = p;  /* Include closing quote */
            
            /* Find next comma */
            while (p < end && *p != ',') p++;
            if (p < end) p++;  /* Skip comma */
            
            /* Create attribute with quoted value (preserve quotes) */
            M3U8Attribute *attr = (M3U8Attribute*)calloc(1, sizeof(M3U8Attribute));
            if (attr) {
                attr->key = str_ndup(key_start, key_end - key_start);
                if (attr->key) normalize_attr_name(attr->key);
                attr->value = str_ndup(val_start, val_end - val_start);
                if (tail) {
                    tail->next = attr;
                    tail = attr;
                } else {
                    head = tail = attr;
                }
            }
        } else {
            /* Unquoted value - find comma */
            while (p < end && *p != ',') p++;
            val_end = p;
            
            /* Trim whitespace */
            while (val_end > val_start && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
            
            M3U8Attribute *attr = (M3U8Attribute*)calloc(1, sizeof(M3U8Attribute));
            if (attr) {
                attr->key = str_ndup(key_start, key_end - key_start);
                if (attr->key) normalize_attr_name(attr->key);
                attr->value = str_ndup(val_start, val_end - val_start);
                if (tail) {
                    tail->next = attr;
                    tail = attr;
                } else {
                    head = tail = attr;
                }
            }
            
            if (p < end) p++;  /* Skip comma */
        }
    }
    
    return head;
}

/* Get attribute value by key from attribute list */
static const char* get_attr(M3U8Attribute *attrs, const char *key) {
    for (M3U8Attribute *a = attrs; a; a = a->next) {
        if (strcmp(a->key, key) == 0) {
            return a->value;
        }
    }
    return NULL;
}

/* Get integer attribute, returns default if not found */
static int get_attr_int(M3U8Attribute *attrs, const char *key, int def) {
    const char *v = get_attr(attrs, key);
    if (v) return atoi(v);
    return def;
}

/* Get long long attribute, returns default if not found */
static long long get_attr_ll(M3U8Attribute *attrs, const char *key, long long def) {
    const char *v = get_attr(attrs, key);
    if (v) return strtoll(v, NULL, 10);
    return def;
}

/* Get double attribute, returns default if not found */
static double get_attr_double(M3U8Attribute *attrs, const char *key, double def) {
    const char *v = get_attr(attrs, key);
    if (v) return atof(v);
    return def;
}

/* Free attribute list */
static void free_attributes(M3U8Attribute *attrs) {
    while (attrs) {
        M3U8Attribute *next = attrs->next;
        free(attrs->key);
        free(attrs->value);
        free(attrs);
        attrs = next;
    }
}

/* Copy attribute value as new string */
static char* get_attr_str(M3U8Attribute *attrs, const char *key) {
    const char *v = get_attr(attrs, key);
    return v ? str_dup(v) : NULL;
}

/* Copy attribute value with quotes removed */
static char* get_attr_str_unquoted(M3U8Attribute *attrs, const char *key) {
    const char *v = get_attr(attrs, key);
    if (!v) return NULL;
    size_t len = strlen(v);
    if (len >= 2) {
        if ((v[0] == '"' && v[len-1] == '"') ||
            (v[0] == '\'' && v[len-1] == '\'')) {
            return str_ndup(v + 1, len - 2);
        }
    }
    return str_dup(v);
}

/* ============================================================================
 * Parser state
 * ========================================================================= */

typedef struct {
    int expect_segment;
    int expect_playlist;
    
    /* Current segment being built */
    M3U8Segment *current_segment;
    
    /* Current key and map (apply to subsequent segments) */
    M3U8Key *current_key;
    M3U8Map *current_map;
    
    /* Cue state */
    int cue_out;
    int cue_out_start;
    int cue_out_explicitly_duration;
    int cue_in;
    int discontinuity;
    int gap;
    char *blackout;
    char *current_cue_out_scte35;
    char *current_cue_out_oatcls_scte35;
    char *current_cue_out_duration;
    char *current_cue_out_elapsedtime;
    M3U8Attribute *asset_metadata;
    char *program_date_time;
    
    /* Date ranges for current segment */
    M3U8DateRange *dateranges;
    
    /* Stream info for variant playlists */
    M3U8Attribute *stream_info;
} ParserState;

/* ============================================================================
 * Tag handlers
 * ========================================================================= */

static void handle_targetduration(M3U8Data *data, const char *line, size_t len) {
    const char *p = line + 22;  /* Skip "#EXT-X-TARGETDURATION:" */
    data->targetduration = atoi(p);
}

static void handle_media_sequence(M3U8Data *data, const char *line, size_t len) {
    const char *p = line + 22;  /* Skip "#EXT-X-MEDIA-SEQUENCE:" */
    data->media_sequence = strtoll(p, NULL, 10);
    data->has_media_sequence = 1;
}

static void handle_discontinuity_sequence(M3U8Data *data, const char *line, size_t len) {
    const char *p = line + 30;  /* Skip "#EXT-X-DISCONTINUITY-SEQUENCE:" */
    data->discontinuity_sequence = strtoll(p, NULL, 10);
}

static void handle_version(M3U8Data *data, const char *line, size_t len) {
    const char *p = line + 15;  /* Skip "#EXT-X-VERSION:" */
    data->version = atoi(p);
}

static void handle_allow_cache(M3U8Data *data, const char *line, size_t len) {
    const char *p = line + 19;  /* Skip "#EXT-X-ALLOW-CACHE:" */
    const char *end = line + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    size_t val_len = end - p;
    data->allow_cache = str_ndup(p, val_len);
    /* Lowercase */
    if (data->allow_cache) {
        for (char *s = data->allow_cache; *s; s++) {
            *s = (char)tolower((unsigned char)*s);
        }
    }
}

static void handle_playlist_type(M3U8Data *data, const char *line, size_t len) {
    const char *p = line + 21;  /* Skip "#EXT-X-PLAYLIST-TYPE:" */
    const char *end = line + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    size_t val_len = end - p;
    data->playlist_type = str_ndup(p, val_len);
    /* Lowercase */
    if (data->playlist_type) {
        for (char *s = data->playlist_type; *s; s++) {
            *s = (char)tolower((unsigned char)*s);
        }
    }
}

static void handle_program_date_time(M3U8Data *data, ParserState *state, const char *line, size_t len) {
    const char *p = line + 25;  /* Skip "#EXT-X-PROGRAM-DATE-TIME:" */
    const char *end = line + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    size_t val_len = end - p;
    
    free(state->program_date_time);
    state->program_date_time = str_ndup(p, val_len);
    
    if (!data->program_date_time) {
        data->program_date_time = str_ndup(p, val_len);
    }
}

static void handle_endlist(M3U8Data *data) {
    data->is_endlist = 1;
}

static void handle_i_frames_only(M3U8Data *data) {
    data->is_i_frames_only = 1;
}

static void handle_independent_segments(M3U8Data *data) {
    data->is_independent_segments = 1;
}

static void handle_images_only(M3U8Data *data) {
    data->is_images_only = 1;
}

static void handle_discontinuity(ParserState *state) {
    state->discontinuity = 1;
}

static void handle_gap(ParserState *state) {
    state->gap = 1;
}

static void handle_cue_in(ParserState *state) {
    state->cue_in = 1;
}

static void handle_cue_span(ParserState *state) {
    state->cue_out = 1;
}

static void handle_blackout(ParserState *state, const char *line, size_t len) {
    free(state->blackout);
    const char *colon = memchr(line, ':', len);
    if (colon) {
        size_t val_len = len - (colon - line) - 1;
        state->blackout = str_ndup(colon + 1, val_len);
    } else {
        /* No parameters, use special marker that Python will convert to True */
        state->blackout = str_dup("__BLACKOUT_TRUE__");
    }
}

static void handle_key(M3U8Data *data, ParserState *state, const char *line, size_t len) {
    const char *params = line + 11;  /* Skip "#EXT-X-KEY:" */
    size_t params_len = len - 11;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8Key *key = (M3U8Key*)calloc(1, sizeof(M3U8Key));
    if (key) {
        /* All key attributes have quotes removed */
        key->method = get_attr_str_unquoted(attrs, "method");
        key->uri = get_attr_str_unquoted(attrs, "uri");
        key->iv = get_attr_str_unquoted(attrs, "iv");
        key->keyformat = get_attr_str_unquoted(attrs, "keyformat");
        key->keyformatversions = get_attr_str_unquoted(attrs, "keyformatversions");
        
        /* Set as current key */
        state->current_key = key;
        
        /* Add to keys list */
        if (data->keys_tail) {
            data->keys_tail->next = key;
            data->keys_tail = key;
        } else {
            data->keys_head = data->keys_tail = key;
        }
    }
    
    free_attributes(attrs);
}

static void handle_session_key(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 19;  /* Skip "#EXT-X-SESSION-KEY:" */
    size_t params_len = len - 19;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8Key *key = (M3U8Key*)calloc(1, sizeof(M3U8Key));
    if (key) {
        /* All key attributes have quotes removed */
        key->method = get_attr_str_unquoted(attrs, "method");
        key->uri = get_attr_str_unquoted(attrs, "uri");
        key->iv = get_attr_str_unquoted(attrs, "iv");
        key->keyformat = get_attr_str_unquoted(attrs, "keyformat");
        key->keyformatversions = get_attr_str_unquoted(attrs, "keyformatversions");
        
        /* Add to session keys list */
        if (data->session_keys_tail) {
            data->session_keys_tail->next = key;
            data->session_keys_tail = key;
        } else {
            data->session_keys_head = data->session_keys_tail = key;
        }
    }
    
    free_attributes(attrs);
}

static void handle_map(M3U8Data *data, ParserState *state, const char *line, size_t len) {
    const char *params = line + 11;  /* Skip "#EXT-X-MAP:" */
    size_t params_len = len - 11;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8Map *map = (M3U8Map*)calloc(1, sizeof(M3U8Map));
    if (map) {
        map->uri = get_attr_str_unquoted(attrs, "uri");
        map->byterange = get_attr_str_unquoted(attrs, "byterange");
        
        /* Set as current map */
        state->current_map = map;
        
        /* Add to segment map list */
        if (data->segment_map_tail) {
            data->segment_map_tail->next = map;
            data->segment_map_tail = map;
        } else {
            data->segment_map_head = data->segment_map_tail = map;
        }
    }
    
    free_attributes(attrs);
}

static void handle_extinf(ParserState *state, const char *line, size_t len) {
    const char *p = line + 8;  /* Skip "#EXTINF:" */
    const char *end = line + len;
    size_t remaining = end - p;
    
    /* Ensure current segment exists */
    if (!state->current_segment) {
        state->current_segment = (M3U8Segment*)calloc(1, sizeof(M3U8Segment));
    }
    
    if (state->current_segment) {
        /* Parse duration */
        char *endptr;
        state->current_segment->duration = strtod(p, &endptr);
        
        /* Parse title (after comma) */
        const char *comma = memchr(p, ',', remaining);
        if (comma && comma < end) {
            comma++;
            while (comma < end && (*comma == ' ' || *comma == '\t')) comma++;
            size_t title_len = end - comma;
            if (title_len > 0) {
                free(state->current_segment->title);
                state->current_segment->title = str_ndup(comma, title_len);
            }
        }
    }
    
    state->expect_segment = 1;
}

static void handle_byterange(ParserState *state, const char *line, size_t len) {
    const char *p = line + 17;  /* Skip "#EXT-X-BYTERANGE:" */
    size_t val_len = len - 17;
    
    /* Ensure current segment exists */
    if (!state->current_segment) {
        state->current_segment = (M3U8Segment*)calloc(1, sizeof(M3U8Segment));
    }
    
    if (state->current_segment) {
        free(state->current_segment->byterange);
        state->current_segment->byterange = str_ndup(p, val_len);
    }
    
    state->expect_segment = 1;
}

static void handle_bitrate(ParserState *state, const char *line, size_t len) {
    const char *p = line + 15;  /* Skip "#EXT-X-BITRATE:" */
    
    /* Ensure current segment exists */
    if (!state->current_segment) {
        state->current_segment = (M3U8Segment*)calloc(1, sizeof(M3U8Segment));
    }
    
    if (state->current_segment) {
        state->current_segment->bitrate = atoi(p);
    }
}

static void handle_cue_out(ParserState *state, const char *line, size_t len) {
    state->cue_out_start = 1;
    state->cue_out = 1;
    
    /* Check for DURATION keyword */
    if (len > 12) {  /* "#EXT-X-CUE-OUT:" = 15 chars */
        const char *upper = line;
        for (size_t i = 0; i < len; i++) {
            char c = line[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == 'D' && len - i >= 8) {
                if (strncmp(line + i, "DURATION", 8) == 0 ||
                    strncmp(line + i, "duration", 8) == 0) {
                    state->cue_out_explicitly_duration = 1;
                    break;
                }
            }
        }
    }
    
    /* Parse attributes if present */
    const char *colon = memchr(line, ':', len);
    if (colon && (size_t)(colon - line) < len - 1) {
        const char *params = colon + 1;
        size_t params_len = len - (params - line);
        
        M3U8Attribute *attrs = parse_attributes(params, params_len);
        
        /* "cue" should have quotes removed per Python's remove_quotes_parser */
        char *cue = get_attr_str_unquoted(attrs, "cue");
        if (cue) {
            free(state->current_cue_out_scte35);
            state->current_cue_out_scte35 = cue;  /* Takes ownership */
        }
        
        const char *duration = get_attr(attrs, "duration");
        if (!duration) duration = get_attr(attrs, "");  /* Bare value */
        if (duration) {
            free(state->current_cue_out_duration);
            state->current_cue_out_duration = str_dup(duration);
        }
        
        free_attributes(attrs);
    }
}

static void handle_cue_out_cont(ParserState *state, const char *line, size_t len) {
    state->cue_out = 1;
    
    const char *colon = memchr(line, ':', len);
    if (!colon) return;
    
    const char *params = colon + 1;
    size_t params_len = len - (params - line);
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    /* Check for "elapsed/duration" format */
    const char *bare = get_attr(attrs, "");
    if (bare && strchr(bare, '/')) {
        const char *slash = strchr(bare, '/');
        free(state->current_cue_out_elapsedtime);
        state->current_cue_out_elapsedtime = str_ndup(bare, slash - bare);
        free(state->current_cue_out_duration);
        state->current_cue_out_duration = str_dup(slash + 1);
    } else if (bare) {
        free(state->current_cue_out_duration);
        state->current_cue_out_duration = str_dup(bare);
    }
    
    /* Per Python's remove_quotes_parser("duration", "elapsedtime", "scte35") */
    char *duration = get_attr_str_unquoted(attrs, "duration");
    if (duration) {
        free(state->current_cue_out_duration);
        state->current_cue_out_duration = duration;  /* Takes ownership */
    }
    
    char *scte35 = get_attr_str_unquoted(attrs, "scte35");
    if (scte35) {
        free(state->current_cue_out_scte35);
        state->current_cue_out_scte35 = scte35;  /* Takes ownership */
    }
    
    char *elapsed = get_attr_str_unquoted(attrs, "elapsedtime");
    if (elapsed) {
        free(state->current_cue_out_elapsedtime);
        state->current_cue_out_elapsedtime = elapsed;  /* Takes ownership */
    }
    
    free_attributes(attrs);
}

static void handle_oatcls_scte35(ParserState *state, const char *line, size_t len) {
    const char *colon = memchr(line, ':', len);
    if (colon) {
        const char *val = colon + 1;
        size_t val_len = len - (val - line);
        free(state->current_cue_out_oatcls_scte35);
        state->current_cue_out_oatcls_scte35 = str_ndup(val, val_len);
        
        if (!state->current_cue_out_scte35) {
            state->current_cue_out_scte35 = str_ndup(val, val_len);
        }
    }
}

static void handle_asset(ParserState *state, const char *line, size_t len) {
    const char *colon = memchr(line, ':', len);
    if (colon) {
        const char *params = colon + 1;
        size_t params_len = len - (params - line);
        
        free_attributes(state->asset_metadata);
        state->asset_metadata = parse_attributes(params, params_len);
    }
}

static void handle_daterange(ParserState *state, const char *line, size_t len) {
    const char *params = line + 17;  /* Skip "#EXT-X-DATERANGE:" */
    size_t params_len = len - 17;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8DateRange *dr = (M3U8DateRange*)calloc(1, sizeof(M3U8DateRange));
    if (dr) {
        /* These 4 should have quotes removed per Python's remove_quotes_parser */
        dr->id = get_attr_str_unquoted(attrs, "id");
        dr->class_name = get_attr_str_unquoted(attrs, "class");
        dr->start_date = get_attr_str_unquoted(attrs, "start_date");
        dr->end_date = get_attr_str_unquoted(attrs, "end_date");
        dr->duration = get_attr_double(attrs, "duration", 0);
        dr->planned_duration = get_attr_double(attrs, "planned_duration", 0);
        dr->scte35_cmd = get_attr_str(attrs, "scte35_cmd");
        dr->scte35_out = get_attr_str(attrs, "scte35_out");
        dr->scte35_in = get_attr_str(attrs, "scte35_in");
        dr->end_on_next = get_attr_str(attrs, "end_on_next");
        
        /* Collect X-* attributes */
        M3U8Attribute *x_head = NULL;
        M3U8Attribute *x_tail = NULL;
        for (M3U8Attribute *a = attrs; a; a = a->next) {
            if (a->key && a->key[0] == 'x' && a->key[1] == '_') {
                M3U8Attribute *x = (M3U8Attribute*)calloc(1, sizeof(M3U8Attribute));
                if (x) {
                    x->key = str_dup(a->key);
                    x->value = str_dup(a->value);
                    if (x_tail) {
                        x_tail->next = x;
                        x_tail = x;
                    } else {
                        x_head = x_tail = x;
                    }
                }
            }
        }
        dr->x_attrs = x_head;
        
        /* Add to state's dateranges list */
        dr->next = state->dateranges;
        state->dateranges = dr;
    }
    
    free_attributes(attrs);
}

static void handle_stream_inf(M3U8Data *data, ParserState *state, const char *line, size_t len) {
    const char *params = line + 18;  /* Skip "#EXT-X-STREAM-INF:" */
    size_t params_len = len - 18;
    
    data->is_variant = 1;
    data->has_media_sequence = 0;  /* media_sequence = None for variants */
    
    free_attributes(state->stream_info);
    state->stream_info = parse_attributes(params, params_len);
    state->expect_playlist = 1;
}

static void handle_i_frame_stream_inf(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 26;  /* Skip "#EXT-X-I-FRAME-STREAM-INF:" */
    size_t params_len = len - 26;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8IFramePlaylist *pl = (M3U8IFramePlaylist*)calloc(1, sizeof(M3U8IFramePlaylist));
    if (pl) {
        pl->uri = get_attr_str_unquoted(attrs, "uri");
        pl->program_id = get_attr_int(attrs, "program_id", 0);
        pl->bandwidth = get_attr_ll(attrs, "bandwidth", 0);
        pl->average_bandwidth = get_attr_ll(attrs, "average_bandwidth", 0);
        pl->resolution = get_attr_str(attrs, "resolution");
        pl->codecs = get_attr_str_unquoted(attrs, "codecs");
        pl->video_range = get_attr_str_unquoted(attrs, "video_range");
        pl->hdcp_level = get_attr_str(attrs, "hdcp_level");
        pl->pathway_id = get_attr_str_unquoted(attrs, "pathway_id");
        pl->stable_variant_id = get_attr_str_unquoted(attrs, "stable_variant_id");
        
        if (data->iframe_playlists_tail) {
            data->iframe_playlists_tail->next = pl;
            data->iframe_playlists_tail = pl;
        } else {
            data->iframe_playlists_head = data->iframe_playlists_tail = pl;
        }
    }
    
    free_attributes(attrs);
}

static void handle_image_stream_inf(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 24;  /* Skip "#EXT-X-IMAGE-STREAM-INF:" */
    size_t params_len = len - 24;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8ImagePlaylist *pl = (M3U8ImagePlaylist*)calloc(1, sizeof(M3U8ImagePlaylist));
    if (pl) {
        pl->uri = get_attr_str_unquoted(attrs, "uri");
        pl->program_id = get_attr_int(attrs, "program_id", 0);
        pl->bandwidth = get_attr_ll(attrs, "bandwidth", 0);
        pl->average_bandwidth = get_attr_ll(attrs, "average_bandwidth", 0);
        pl->resolution = get_attr_str(attrs, "resolution");
        pl->codecs = get_attr_str_unquoted(attrs, "codecs");
        pl->pathway_id = get_attr_str_unquoted(attrs, "pathway_id");
        pl->stable_variant_id = get_attr_str_unquoted(attrs, "stable_variant_id");
        
        if (data->image_playlists_tail) {
            data->image_playlists_tail->next = pl;
            data->image_playlists_tail = pl;
        } else {
            data->image_playlists_head = data->image_playlists_tail = pl;
        }
    }
    
    free_attributes(attrs);
}

static void handle_media(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 13;  /* Skip "#EXT-X-MEDIA:" */
    size_t params_len = len - 13;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8Media *media = (M3U8Media*)calloc(1, sizeof(M3U8Media));
    if (media) {
        media->type = get_attr_str(attrs, "type");
        /* These attributes should have quotes removed per remove_quotes_parser */
        media->uri = get_attr_str_unquoted(attrs, "uri");
        media->group_id = get_attr_str_unquoted(attrs, "group_id");
        media->language = get_attr_str_unquoted(attrs, "language");
        media->assoc_language = get_attr_str_unquoted(attrs, "assoc_language");
        media->name = get_attr_str_unquoted(attrs, "name");
        media->default_val = get_attr_str(attrs, "default");
        media->autoselect = get_attr_str(attrs, "autoselect");
        media->forced = get_attr_str(attrs, "forced");
        media->instream_id = get_attr_str_unquoted(attrs, "instream_id");
        media->characteristics = get_attr_str_unquoted(attrs, "characteristics");
        media->channels = get_attr_str_unquoted(attrs, "channels");
        media->stable_rendition_id = get_attr_str_unquoted(attrs, "stable_rendition_id");
        
        if (data->media_tail) {
            data->media_tail->next = media;
            data->media_tail = media;
        } else {
            data->media_head = data->media_tail = media;
        }
    }
    
    free_attributes(attrs);
}

static void handle_start(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 13;  /* Skip "#EXT-X-START:" */
    size_t params_len = len - 13;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    data->start_time_offset = get_attr_double(attrs, "time_offset", 0);
    data->start_precise = get_attr_str(attrs, "precise");
    data->has_start = 1;
    
    free_attributes(attrs);
}

static void handle_server_control(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 22;  /* Skip "#EXT-X-SERVER-CONTROL:" */
    size_t params_len = len - 22;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    data->server_control_can_block_reload = get_attr_str(attrs, "can_block_reload");
    data->server_control_hold_back = get_attr_double(attrs, "hold_back", 0);
    data->server_control_part_hold_back = get_attr_double(attrs, "part_hold_back", 0);
    data->server_control_can_skip_until = get_attr_double(attrs, "can_skip_until", 0);
    data->server_control_can_skip_dateranges = get_attr_str(attrs, "can_skip_dateranges");
    data->has_server_control = 1;
    
    free_attributes(attrs);
}

static void handle_part_inf(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 16;  /* Skip "#EXT-X-PART-INF:" */
    size_t params_len = len - 16;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    data->part_inf_part_target = get_attr_double(attrs, "part_target", 0);
    data->has_part_inf = 1;
    
    free_attributes(attrs);
}

static void handle_skip(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 12;  /* Skip "#EXT-X-SKIP:" */
    size_t params_len = len - 12;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    data->skip_skipped_segments = get_attr_int(attrs, "skipped_segments", 0);
    data->skip_recently_removed_dateranges = get_attr_str_unquoted(attrs, "recently_removed_dateranges");
    data->has_skip = 1;
    
    free_attributes(attrs);
}

static void handle_rendition_report(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 24;  /* Skip "#EXT-X-RENDITION-REPORT:" */
    size_t params_len = len - 24;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8RenditionReport *rr = (M3U8RenditionReport*)calloc(1, sizeof(M3U8RenditionReport));
    if (rr) {
        rr->uri = get_attr_str_unquoted(attrs, "uri");
        const char *last_msn = get_attr(attrs, "last_msn");
        if (last_msn) {
            rr->last_msn = strtoll(last_msn, NULL, 10);
            rr->has_last_msn = 1;
        }
        const char *last_part = get_attr(attrs, "last_part");
        if (last_part) {
            rr->last_part = strtoll(last_part, NULL, 10);
            rr->has_last_part = 1;
        }
        
        if (data->rendition_reports_tail) {
            data->rendition_reports_tail->next = rr;
            data->rendition_reports_tail = rr;
        } else {
            data->rendition_reports_head = data->rendition_reports_tail = rr;
        }
    }
    
    free_attributes(attrs);
}

static void handle_session_data(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 20;  /* Skip "#EXT-X-SESSION-DATA:" */
    size_t params_len = len - 20;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8SessionData *sd = (M3U8SessionData*)calloc(1, sizeof(M3U8SessionData));
    if (sd) {
        sd->data_id = get_attr_str_unquoted(attrs, "data_id");
        sd->value = get_attr_str_unquoted(attrs, "value");
        sd->uri = get_attr_str_unquoted(attrs, "uri");
        sd->language = get_attr_str_unquoted(attrs, "language");
        
        if (data->session_data_tail) {
            data->session_data_tail->next = sd;
            data->session_data_tail = sd;
        } else {
            data->session_data_head = data->session_data_tail = sd;
        }
    }
    
    free_attributes(attrs);
}

static void handle_preload_hint(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 20;  /* Skip "#EXT-X-PRELOAD-HINT:" */
    size_t params_len = len - 20;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    data->preload_hint_type = get_attr_str(attrs, "type");
    data->preload_hint_uri = get_attr_str_unquoted(attrs, "uri");
    
    const char *bs = get_attr(attrs, "byterange_start");
    if (bs) {
        data->preload_hint_byterange_start = atoi(bs);
        data->has_preload_hint_byterange_start = 1;
    }
    
    const char *bl = get_attr(attrs, "byterange_length");
    if (bl) {
        data->preload_hint_byterange_length = atoi(bl);
        data->has_preload_hint_byterange_length = 1;
    }
    
    data->has_preload_hint = 1;
    
    free_attributes(attrs);
}

static void handle_content_steering(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 24;  /* Skip "#EXT-X-CONTENT-STEERING:" */
    size_t params_len = len - 24;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    data->content_steering_server_uri = get_attr_str_unquoted(attrs, "server_uri");
    data->content_steering_pathway_id = get_attr_str_unquoted(attrs, "pathway_id");
    data->has_content_steering = 1;
    
    free_attributes(attrs);
}

static void handle_tiles(M3U8Data *data, const char *line, size_t len) {
    const char *params = line + 13;  /* Skip "#EXT-X-TILES:" */
    size_t params_len = len - 13;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8Tiles *tiles = (M3U8Tiles*)calloc(1, sizeof(M3U8Tiles));
    if (tiles) {
        tiles->resolution = get_attr_str(attrs, "resolution");
        tiles->layout = get_attr_str(attrs, "layout");
        tiles->duration = get_attr_double(attrs, "duration", 0);
        tiles->uri = get_attr_str_unquoted(attrs, "uri");
        
        if (data->tiles_tail) {
            data->tiles_tail->next = tiles;
            data->tiles_tail = tiles;
        } else {
            data->tiles_head = data->tiles_tail = tiles;
        }
    }
    
    free_attributes(attrs);
}

static void handle_part(ParserState *state, const char *line, size_t len) {
    const char *params = line + 12;  /* Skip "#EXT-X-PART:" */
    size_t params_len = len - 12;
    
    M3U8Attribute *attrs = parse_attributes(params, params_len);
    
    M3U8Part *part = (M3U8Part*)calloc(1, sizeof(M3U8Part));
    if (part) {
        part->uri = get_attr_str_unquoted(attrs, "uri");
        part->duration = get_attr_double(attrs, "duration", 0);
        part->byterange = get_attr_str(attrs, "byterange");
        part->independent = get_attr_str(attrs, "independent");
        part->gap = get_attr_str(attrs, "gap");
        part->gap_tag = state->gap;
        
        /* Transfer dateranges */
        part->dateranges = state->dateranges;
        state->dateranges = NULL;
        state->gap = 0;
        
        /* Ensure current segment exists */
        if (!state->current_segment) {
            state->current_segment = (M3U8Segment*)calloc(1, sizeof(M3U8Segment));
        }
        
        /* Add to segment's parts list */
        if (state->current_segment) {
            M3U8Part *tail = state->current_segment->parts;
            if (tail) {
                while (tail->next) tail = tail->next;
                tail->next = part;
            } else {
                state->current_segment->parts = part;
            }
        }
    }
    
    free_attributes(attrs);
}

/* Finalize a segment when URI line is encountered */
static void finalize_segment(M3U8Data *data, ParserState *state, const char *uri, size_t uri_len) {
    M3U8Segment *seg = state->current_segment;
    if (!seg) {
        seg = (M3U8Segment*)calloc(1, sizeof(M3U8Segment));
    }
    
    if (seg) {
        seg->uri = str_ndup(uri, uri_len);
        
        /* Apply state */
        seg->discontinuity = state->discontinuity;
        seg->cue_in = state->cue_in;
        seg->cue_out = state->cue_out;
        seg->cue_out_start = state->cue_out_start;
        seg->cue_out_explicitly_duration = state->cue_out_explicitly_duration;
        seg->gap_tag = state->gap;
        
        if (state->blackout) {
            seg->blackout = str_dup(state->blackout);
        }
        
        if (state->program_date_time) {
            seg->program_date_time = str_dup(state->program_date_time);
            free(state->program_date_time);
            state->program_date_time = NULL;
        }
        
        /* SCTE35 handling */
        if (state->cue_out) {
            seg->scte35 = state->current_cue_out_scte35 ? str_dup(state->current_cue_out_scte35) : NULL;
            seg->oatcls_scte35 = state->current_cue_out_oatcls_scte35 ? str_dup(state->current_cue_out_oatcls_scte35) : NULL;
            seg->scte35_duration = state->current_cue_out_duration ? str_dup(state->current_cue_out_duration) : NULL;
            seg->scte35_elapsedtime = state->current_cue_out_elapsedtime ? str_dup(state->current_cue_out_elapsedtime) : NULL;
        } else {
            /* Pop SCTE35 data on cue_in */
            seg->scte35 = state->current_cue_out_scte35;
            seg->oatcls_scte35 = state->current_cue_out_oatcls_scte35;
            seg->scte35_duration = state->current_cue_out_duration;
            seg->scte35_elapsedtime = state->current_cue_out_elapsedtime;
            state->current_cue_out_scte35 = NULL;
            state->current_cue_out_oatcls_scte35 = NULL;
            state->current_cue_out_duration = NULL;
            state->current_cue_out_elapsedtime = NULL;
        }
        
        /* Asset metadata */
        if (state->asset_metadata) {
            if (state->cue_out) {
                /* Copy */
                M3U8Attribute *copy_head = NULL, *copy_tail = NULL;
                for (M3U8Attribute *a = state->asset_metadata; a; a = a->next) {
                    M3U8Attribute *c = (M3U8Attribute*)calloc(1, sizeof(M3U8Attribute));
                    if (c) {
                        c->key = str_dup(a->key);
                        c->value = str_dup(a->value);
                        if (copy_tail) { copy_tail->next = c; copy_tail = c; }
                        else { copy_head = copy_tail = c; }
                    }
                }
                seg->asset_metadata = copy_head;
            } else {
                seg->asset_metadata = state->asset_metadata;
                state->asset_metadata = NULL;
            }
        }
        
        /* Key and map */
        seg->key = state->current_key;
        seg->init_section = state->current_map;
        
        /* Dateranges */
        seg->dateranges = state->dateranges;
        state->dateranges = NULL;
        
        /* Add to list */
        if (data->segments_tail) {
            data->segments_tail->next = seg;
            data->segments_tail = seg;
        } else {
            data->segments_head = data->segments_tail = seg;
        }
        
        /* Reset state - these are per-segment flags that get "popped" after each segment */
        state->current_segment = NULL;
        state->expect_segment = 0;
        state->discontinuity = 0;
        state->cue_in = 0;
        state->cue_out = 0;           /* Pop cue_out - reset after each segment */
        state->cue_out_start = 0;
        state->cue_out_explicitly_duration = 0;
        state->gap = 0;
        free(state->blackout);
        state->blackout = NULL;
    }
}

/* Finalize a variant playlist when URI line is encountered */
static void finalize_playlist(M3U8Data *data, ParserState *state, const char *uri, size_t uri_len) {
    M3U8Playlist *pl = (M3U8Playlist*)calloc(1, sizeof(M3U8Playlist));
    
    if (pl && state->stream_info) {
        pl->uri = str_ndup(uri, uri_len);
        pl->program_id = get_attr_int(state->stream_info, "program_id", 0);
        
        const char *bw = get_attr(state->stream_info, "bandwidth");
        if (bw) {
            pl->bandwidth = (long long)strtod(bw, NULL);  /* Handle float bandwidth */
        }
        
        pl->average_bandwidth = get_attr_ll(state->stream_info, "average_bandwidth", 0);
        /* resolution is NOT unquoted by remove_quotes_parser */
        pl->resolution = get_attr_str(state->stream_info, "resolution");
        /* These ARE unquoted by remove_quotes_parser */
        pl->codecs = get_attr_str_unquoted(state->stream_info, "codecs");
        pl->frame_rate = get_attr_double(state->stream_info, "frame_rate", 0);
        pl->video = get_attr_str_unquoted(state->stream_info, "video");
        pl->audio = get_attr_str_unquoted(state->stream_info, "audio");
        pl->subtitles = get_attr_str_unquoted(state->stream_info, "subtitles");
        pl->video_range = get_attr_str_unquoted(state->stream_info, "video_range");
        pl->hdcp_level = get_attr_str(state->stream_info, "hdcp_level");
        pl->pathway_id = get_attr_str_unquoted(state->stream_info, "pathway_id");
        pl->stable_variant_id = get_attr_str_unquoted(state->stream_info, "stable_variant_id");
        pl->req_video_layout = get_attr_str(state->stream_info, "req_video_layout");
        
        /* Handle closed_captions specially (NOT unquoted - preserves quotes or NONE) */
        const char *cc = get_attr(state->stream_info, "closed_captions");
        if (cc) {
            pl->closed_captions = str_dup(cc);
        }
        
        /* Add to list */
        if (data->playlists_tail) {
            data->playlists_tail->next = pl;
            data->playlists_tail = pl;
        } else {
            data->playlists_head = data->playlists_tail = pl;
        }
    }
    
    /* Reset state */
    free_attributes(state->stream_info);
    state->stream_info = NULL;
    state->expect_playlist = 0;
}

/* ============================================================================
 * Main parser
 * ========================================================================= */

M3U8Data* m3u8_parse(const char *content, size_t length) {
    if (!content || length == 0) return NULL;
    
    M3U8Data *data = (M3U8Data*)calloc(1, sizeof(M3U8Data));
    if (!data) return NULL;
    
    ParserState state = {0};
    
    const char *cursor = content;
    const char *end = content + length;
    
    while (cursor < end) {
        /* Skip leading whitespace */
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) cursor++;
        if (cursor >= end) break;
        
        /* Find end of line */
        const char *line_end = find_eol(cursor, end);
        size_t line_len = line_end - cursor;
        
        /* Trim trailing whitespace */
        while (line_len > 0 && (cursor[line_len-1] == ' ' || cursor[line_len-1] == '\t' || cursor[line_len-1] == '\r')) {
            line_len--;
        }
        
        if (line_len == 0) {
            cursor = line_end + 1;
            continue;
        }
        
        /* Dispatch based on line content */
        if (cursor[0] == '#') {
            /* Tag line */
            if (starts_with(cursor, line_len, "#EXTM3U")) {
                /* Valid playlist header, ignore */
            }
            else if (starts_with(cursor, line_len, "#EXTINF:")) {
                handle_extinf(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-TARGETDURATION:")) {
                handle_targetduration(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-MEDIA-SEQUENCE:")) {
                handle_media_sequence(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-DISCONTINUITY-SEQUENCE:")) {
                handle_discontinuity_sequence(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-VERSION:")) {
                handle_version(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-ALLOW-CACHE:")) {
                handle_allow_cache(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-PLAYLIST-TYPE:")) {
                handle_playlist_type(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-PROGRAM-DATE-TIME:")) {
                handle_program_date_time(data, &state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-ENDLIST")) {
                handle_endlist(data);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-I-FRAMES-ONLY")) {
                handle_i_frames_only(data);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-INDEPENDENT-SEGMENTS")) {
                handle_independent_segments(data);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-IMAGES-ONLY")) {
                handle_images_only(data);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-DISCONTINUITY") && 
                     !starts_with(cursor, line_len, "#EXT-X-DISCONTINUITY-SEQUENCE")) {
                handle_discontinuity(&state);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-GAP")) {
                handle_gap(&state);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-BLACKOUT")) {
                handle_blackout(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-CUE-IN")) {
                handle_cue_in(&state);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-CUE-SPAN")) {
                handle_cue_span(&state);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-CUE-OUT-CONT")) {
                handle_cue_out_cont(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-CUE-OUT")) {
                handle_cue_out(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-OATCLS-SCTE35:")) {
                handle_oatcls_scte35(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-ASSET:")) {
                handle_asset(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-KEY:")) {
                handle_key(data, &state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-SESSION-KEY:")) {
                handle_session_key(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-MAP:")) {
                handle_map(data, &state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-BYTERANGE:")) {
                handle_byterange(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-BITRATE:")) {
                handle_bitrate(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-DATERANGE:")) {
                handle_daterange(&state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-STREAM-INF:")) {
                handle_stream_inf(data, &state, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-I-FRAME-STREAM-INF:")) {
                handle_i_frame_stream_inf(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-IMAGE-STREAM-INF:")) {
                handle_image_stream_inf(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-MEDIA:")) {
                handle_media(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-START:")) {
                handle_start(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-SERVER-CONTROL:")) {
                handle_server_control(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-PART-INF:")) {
                handle_part_inf(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-SKIP:")) {
                handle_skip(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-RENDITION-REPORT:")) {
                handle_rendition_report(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-SESSION-DATA:")) {
                handle_session_data(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-PRELOAD-HINT:")) {
                handle_preload_hint(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-CONTENT-STEERING:")) {
                handle_content_steering(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-TILES:")) {
                handle_tiles(data, cursor, line_len);
            }
            else if (starts_with(cursor, line_len, "#EXT-X-PART:")) {
                handle_part(&state, cursor, line_len);
            }
            /* Unknown tags are ignored (like comments) */
        }
        else {
            /* URI line */
            if (state.expect_segment) {
                finalize_segment(data, &state, cursor, line_len);
            }
            else if (state.expect_playlist) {
                finalize_playlist(data, &state, cursor, line_len);
            }
            /* Other non-comment lines are ignored */
        }
        
        /* Move to next line */
        cursor = line_end;
        if (cursor < end && *cursor == '\r') cursor++;
        if (cursor < end && *cursor == '\n') cursor++;
    }
    
    /* Handle partial segment at end */
    if (state.current_segment) {
        if (data->segments_tail) {
            data->segments_tail->next = state.current_segment;
            data->segments_tail = state.current_segment;
        } else {
            data->segments_head = data->segments_tail = state.current_segment;
        }
    }
    
    /* Clean up state */
    free(state.program_date_time);
    free(state.blackout);
    free(state.current_cue_out_scte35);
    free(state.current_cue_out_oatcls_scte35);
    free(state.current_cue_out_duration);
    free(state.current_cue_out_elapsedtime);
    free_attributes(state.stream_info);
    free_attributes(state.asset_metadata);
    
    /* Free dateranges not attached to a segment */
    while (state.dateranges) {
        M3U8DateRange *next = state.dateranges->next;
        free(state.dateranges->id);
        free(state.dateranges->class_name);
        free(state.dateranges->start_date);
        free(state.dateranges->end_date);
        free(state.dateranges->scte35_cmd);
        free(state.dateranges->scte35_out);
        free(state.dateranges->scte35_in);
        free(state.dateranges->end_on_next);
        free_attributes(state.dateranges->x_attrs);
        free(state.dateranges);
        state.dateranges = next;
    }
    
    return data;
}

/* ============================================================================
 * Memory cleanup
 * ========================================================================= */

static void free_dateranges(M3U8DateRange *dr) {
    while (dr) {
        M3U8DateRange *next = dr->next;
        free(dr->id);
        free(dr->class_name);
        free(dr->start_date);
        free(dr->end_date);
        free(dr->scte35_cmd);
        free(dr->scte35_out);
        free(dr->scte35_in);
        free(dr->end_on_next);
        free_attributes(dr->x_attrs);
        free(dr);
        dr = next;
    }
}

static void free_parts(M3U8Part *part) {
    while (part) {
        M3U8Part *next = part->next;
        free(part->uri);
        free(part->byterange);
        free(part->independent);
        free(part->gap);
        free_dateranges(part->dateranges);
        free(part);
        part = next;
    }
}

void m3u8_free(M3U8Data *data) {
    if (!data) return;
    
    /* Free strings */
    free(data->allow_cache);
    free(data->playlist_type);
    free(data->program_date_time);
    free(data->start_precise);
    free(data->server_control_can_block_reload);
    free(data->server_control_can_skip_dateranges);
    free(data->skip_recently_removed_dateranges);
    free(data->preload_hint_type);
    free(data->preload_hint_uri);
    free(data->content_steering_server_uri);
    free(data->content_steering_pathway_id);
    
    /* Free segments */
    M3U8Segment *seg = data->segments_head;
    while (seg) {
        M3U8Segment *next = seg->next;
        free(seg->title);
        free(seg->uri);
        free(seg->byterange);
        free(seg->program_date_time);
        free(seg->scte35);
        free(seg->oatcls_scte35);
        free(seg->scte35_duration);
        free(seg->scte35_elapsedtime);
        free(seg->blackout);
        free_attributes(seg->asset_metadata);
        free_dateranges(seg->dateranges);
        free_parts(seg->parts);
        free(seg);
        seg = next;
    }
    
    /* Free playlists */
    M3U8Playlist *pl = data->playlists_head;
    while (pl) {
        M3U8Playlist *next = pl->next;
        free(pl->uri);
        free(pl->resolution);
        free(pl->codecs);
        free(pl->video);
        free(pl->audio);
        free(pl->subtitles);
        free(pl->closed_captions);
        free(pl->video_range);
        free(pl->hdcp_level);
        free(pl->pathway_id);
        free(pl->stable_variant_id);
        free(pl->req_video_layout);
        free(pl);
        pl = next;
    }
    
    /* Free iframe playlists */
    M3U8IFramePlaylist *ifr = data->iframe_playlists_head;
    while (ifr) {
        M3U8IFramePlaylist *next = ifr->next;
        free(ifr->uri);
        free(ifr->resolution);
        free(ifr->codecs);
        free(ifr->video_range);
        free(ifr->hdcp_level);
        free(ifr->pathway_id);
        free(ifr->stable_variant_id);
        free(ifr);
        ifr = next;
    }
    
    /* Free image playlists */
    M3U8ImagePlaylist *img = data->image_playlists_head;
    while (img) {
        M3U8ImagePlaylist *next = img->next;
        free(img->uri);
        free(img->resolution);
        free(img->codecs);
        free(img->pathway_id);
        free(img->stable_variant_id);
        free(img);
        img = next;
    }
    
    /* Free media */
    M3U8Media *media = data->media_head;
    while (media) {
        M3U8Media *next = media->next;
        free(media->type);
        free(media->uri);
        free(media->group_id);
        free(media->language);
        free(media->assoc_language);
        free(media->name);
        free(media->default_val);
        free(media->autoselect);
        free(media->forced);
        free(media->instream_id);
        free(media->characteristics);
        free(media->channels);
        free(media->stable_rendition_id);
        free(media);
        media = next;
    }
    
    /* Free keys */
    M3U8Key *key = data->keys_head;
    while (key) {
        M3U8Key *next = key->next;
        free(key->method);
        free(key->uri);
        free(key->iv);
        free(key->keyformat);
        free(key->keyformatversions);
        free(key);
        key = next;
    }
    
    /* Free session keys */
    key = data->session_keys_head;
    while (key) {
        M3U8Key *next = key->next;
        free(key->method);
        free(key->uri);
        free(key->iv);
        free(key->keyformat);
        free(key->keyformatversions);
        free(key);
        key = next;
    }
    
    /* Free segment maps */
    M3U8Map *map = data->segment_map_head;
    while (map) {
        M3U8Map *next = map->next;
        free(map->uri);
        free(map->byterange);
        free(map);
        map = next;
    }
    
    /* Free rendition reports */
    M3U8RenditionReport *rr = data->rendition_reports_head;
    while (rr) {
        M3U8RenditionReport *next = rr->next;
        free(rr->uri);
        free(rr);
        rr = next;
    }
    
    /* Free session data */
    M3U8SessionData *sd = data->session_data_head;
    while (sd) {
        M3U8SessionData *next = sd->next;
        free(sd->data_id);
        free(sd->value);
        free(sd->uri);
        free(sd->language);
        free(sd);
        sd = next;
    }
    
    /* Free tiles */
    M3U8Tiles *tiles = data->tiles_head;
    while (tiles) {
        M3U8Tiles *next = tiles->next;
        free(tiles->resolution);
        free(tiles->layout);
        free(tiles->uri);
        free(tiles);
        tiles = next;
    }
    
    free(data);
}
