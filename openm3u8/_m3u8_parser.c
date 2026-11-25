/*
 * Copyright 2014 Globo.com Player authors. All rights reserved.
 * Modifications Copyright (c) 2026 Wurl.
 * Use of this source code is governed by a MIT License
 * license that can be found in the LICENSE file.
 *
 * C extension for m3u8 parser - provides optimized parsing of M3U8 playlists.
 *
 * This module implements the same parsing logic as openm3u8/parser.py in C
 * for improved performance. Output is kept compatible with the pure-Python
 * implementation while targeting the Python 3.10+ stable ABI (Py_LIMITED_API).
 *
 * Key optimizations:
 * - Zero-copy SIMD (memchr) line scanning without per-line buffer copies
 * - O(1) tag dispatch via #EXTINF fast path and #EXT-X- character/length switch
 * - Parser state stored directly in C struct (ParseContext) with lazy Python
 *   state dict synchronization only when custom_tags_parser is callable
 * - Pre-built immutable segment_template dict copied via PyDict_Copy (avoids
 *   dict resizes and ~13 redundant PyDict_SetItem calls per segment)
 * - Deferred #EXTINF segment dict creation until URI line / materialization
 * - Lazy current_program_date_time advancement (avoids wasted timedelta math
 *   when per-segment #EXT-X-PROGRAM-DATE-TIME tags are present)
 * - Per-parse 2-slot timedelta cache and 1-entry #EXTINF duration PyFloat cache
 * - Exact IEEE 754 decimal float parser and int64 parser with CPython fallbacks
 * - Pre-interned attribute schema keys and single-pass key unquoting + caching
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * ASCII whitespace/case helpers for RFC 8216 syntax parsing.
 * Avoids C locale-dependent ctype lookups and handles unsigned char safely.
 */
static inline int
ascii_isspace(unsigned char c)
{
    return c == ' '  || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}

static inline unsigned char
ascii_tolower(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

/*
 * Case-insensitive match between a raw buffer (already trimmed, length == key_len)
 * and a lowercase/underscore normalized key literal.
 * Treats '-' as '_'.
 */
static inline int
buffer_matches_key(const char *buf, size_t len, const char *key)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '-') {
            c = '_';
        } else if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + ('a' - 'A'));
        }
        if (c != (unsigned char)key[i]) {
            return 0;
        }
    }
    return 1;
}

/*
 * Zero-copy ASCII case-insensitive substring check.
 * needle must be uppercase ASCII of length needle_len > 0.
 */
static inline int
ascii_contains_ci_upper(const char *haystack, size_t hay_len,
                        const char *needle_upper, size_t needle_len)
{
    if (needle_len == 0) {
        return 1;
    }
    if (hay_len < needle_len) {
        return 0;
    }
    size_t limit = hay_len - needle_len;
    unsigned char first_u = (unsigned char)needle_upper[0];
    unsigned char first_l = (unsigned char)(first_u + ('a' - 'A'));

    for (size_t i = 0; i <= limit; i++) {
        unsigned char c = (unsigned char)haystack[i];
        if (c == first_u || c == first_l) {
            size_t j = 1;
            for (; j < needle_len; j++) {
                unsigned char hc = (unsigned char)haystack[i + j];
                if (hc >= 'a' && hc <= 'z') {
                    hc = (unsigned char)(hc - ('a' - 'A'));
                }
                if (hc != (unsigned char)needle_upper[j]) {
                    break;
                }
            }
            if (j == needle_len) {
                return 1;
            }
        }
    }
    return 0;
}

/* Compatibility shims for Py_NewRef/Py_XNewRef (added in Python 3.10). */
#if PY_VERSION_HEX < 0x030a00f0
static inline PyObject *
Py_NewRef(PyObject *obj)
{
    Py_INCREF(obj);
    return obj;
}

static inline PyObject *
Py_XNewRef(PyObject *obj)
{
    Py_XINCREF(obj);
    return obj;
}
#endif

/* Safe owned-pointer replacement (decrefs old after assigning new). */
static inline void
pyobj_replace(PyObject **slot, PyObject *new_val)
{
    PyObject *old = *slot;
    *slot = new_val;
    Py_XDECREF(old);
}

/*
 * Exact IEEE 754 powers of 10 from 10^0 to 10^22.
 * All 23 values are exactly representable in IEEE 754 binary64 because
 * 5^22 < 2^53. Combined with an integer mantissa < 10^15 < 2^53,
 * `(double)mantissa / POW10[frac_digits]` performs a single IEEE 754
 * division and is guaranteed bit-for-bit identical to strtod / Python float().
 */
static const double EXACT_POW10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/*
 * Fast exact decimal float parser for simple non-negative decimals.
 * Returns 1 and writes *out on fast-path hit; returns 0 to defer to
 * PyOS_string_to_double (signs, exponents, >15 sig digits, >22 frac digits, etc.).
 */
static inline int
fast_parse_double(const char *s, size_t len, double *out)
{
    while (len > 0 && ascii_isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && ascii_isspace((unsigned char)s[len - 1])) {
        len--;
    }
    if (len == 0) {
        return 0;
    }

    uint64_t mantissa = 0;
    int sig_digits = 0;
    int frac_digits = 0;
    int seen_dot = 0;
    int seen_digit = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= '0' && c <= '9') {
            seen_digit = 1;
            unsigned int d = (unsigned int)(c - '0');
            if (seen_dot) {
                frac_digits++;
                if (frac_digits > 22) {
                    return 0;
                }
            }
            if (mantissa != 0 || d != 0) {
                sig_digits++;
                if (sig_digits > 15) {
                    return 0;
                }
                mantissa = mantissa * 10u + d;
            }
        } else if (c == '.' && !seen_dot) {
            seen_dot = 1;
        } else {
            return 0;
        }
    }

    if (!seen_digit) {
        return 0;
    }
    if (mantissa == 0) {
        *out = 0.0;
        return 1;
    }
    if (frac_digits == 0) {
        *out = (double)mantissa;
        return 1;
    }
    *out = (double)mantissa / EXACT_POW10[frac_digits];
    return 1;
}

/*
 * Parse double from (s, len) using fast_parse_double with fallback to
 * PyOS_string_to_double. Returns 0 on success, -1 on parse failure
 * (without leaving a Python exception set).
 */
static inline int
parse_double_slice(const char *s, size_t len, double *out)
{
    if (fast_parse_double(s, len, out)) {
        return 0;
    }
    char stack_buf[64];
    char *buf = stack_buf;
    int use_heap = (len >= sizeof(stack_buf));
    if (use_heap) {
        buf = PyMem_Malloc(len + 1);
        if (buf == NULL) {
            PyErr_Clear();
            return -1;
        }
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    double v = PyOS_string_to_double(buf, NULL, NULL);
    if (use_heap) {
        PyMem_Free(buf);
    }
    if (v == -1.0 && PyErr_Occurred()) {
        PyErr_Clear();
        return -1;
    }
    *out = v;
    return 0;
}

/*
 * Fast decimal int64 parser for typical HLS integers (<= 18 digits).
 * Returns 1 and writes *out on hit; returns 0 to defer to PyLong_FromString.
 */
static inline int
fast_parse_int64(const char *s, size_t len, long long *out)
{
    while (len > 0 && ascii_isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && ascii_isspace((unsigned char)s[len - 1])) {
        len--;
    }
    if (len == 0) {
        return 0;
    }

    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
        len--;
    } else if (*s == '+') {
        s++;
        len--;
    }
    if (len == 0 || len > 18) {
        return 0;
    }

    long long acc = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        acc = acc * 10 + (long long)(c - '0');
    }
    *out = neg ? -acc : acc;
    return 1;
}

/*
 * Parse integer slice into a Python int.
 * Returns new ref on success, NULL (with NO error set) on invalid integer,
 * or NULL with MemoryError set on OOM.
 */
static inline PyObject *
parse_int_slice(const char *s, size_t len)
{
    long long val;
    if (fast_parse_int64(s, len, &val)) {
        return PyLong_FromLongLong(val);
    }
    char stack_buf[64];
    char *buf = stack_buf;
    int use_heap = (len >= sizeof(stack_buf));
    if (use_heap) {
        buf = PyMem_Malloc(len + 1);
        if (buf == NULL) {
            return PyErr_NoMemory();
        }
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    PyObject *res = PyLong_FromString(buf, NULL, 10);
    if (use_heap) {
        PyMem_Free(buf);
    }
    if (res == NULL) {
        if (PyErr_ExceptionMatches(PyExc_ValueError)) {
            PyErr_Clear();
        }
    }
    return res;
}

/*
 * X-macro for interned strings (dict keys, attribute schema keys, common literals).
 */
#define INTERNED_STRINGS(X) \
    /* Common literals */ \
    X(str_empty, "") \
    /* Core parsing keys */ \
    X(str_segment, "segment") \
    X(str_segments, "segments") \
    X(str_duration, "duration") \
    X(str_uri, "uri") \
    X(str_title, "title") \
    X(str_expect_segment, "expect_segment") \
    X(str_expect_playlist, "expect_playlist") \
    X(str_current_key, "current_key") \
    X(str_keys, "keys") \
    X(str_cue_out, "cue_out") \
    X(str_cue_in, "cue_in") \
    /* Segment/state keys */ \
    X(str_program_date_time, "program_date_time") \
    X(str_current_program_date_time, "current_program_date_time") \
    X(str_cue_out_start, "cue_out_start") \
    X(str_cue_out_explicitly_duration, "cue_out_explicitly_duration") \
    X(str_current_cue_out_scte35, "current_cue_out_scte35") \
    X(str_current_cue_out_oatcls_scte35, "current_cue_out_oatcls_scte35") \
    X(str_current_cue_out_duration, "current_cue_out_duration") \
    X(str_current_cue_out_elapsedtime, "current_cue_out_elapsedtime") \
    X(str_scte35, "scte35") \
    X(str_oatcls_scte35, "oatcls_scte35") \
    X(str_scte35_duration, "scte35_duration") \
    X(str_scte35_elapsedtime, "scte35_elapsedtime") \
    X(str_asset_metadata, "asset_metadata") \
    X(str_discontinuity, "discontinuity") \
    X(str_key, "key") \
    X(str_current_segment_map, "current_segment_map") \
    X(str_init_section, "init_section") \
    X(str_dateranges, "dateranges") \
    X(str_gap, "gap") \
    X(str_gap_tag, "gap_tag") \
    X(str_blackout, "blackout") \
    X(str_byterange, "byterange") \
    X(str_bitrate, "bitrate") \
    /* Data dict keys */ \
    X(str_playlists, "playlists") \
    X(str_iframe_playlists, "iframe_playlists") \
    X(str_image_playlists, "image_playlists") \
    X(str_tiles, "tiles") \
    X(str_media, "media") \
    X(str_rendition_reports, "rendition_reports") \
    X(str_session_data, "session_data") \
    X(str_session_keys, "session_keys") \
    X(str_segment_map, "segment_map") \
    X(str_skip, "skip") \
    X(str_part_inf, "part_inf") \
    X(str_is_variant, "is_variant") \
    X(str_is_endlist, "is_endlist") \
    X(str_is_i_frames_only, "is_i_frames_only") \
    X(str_is_independent_segments, "is_independent_segments") \
    X(str_is_images_only, "is_images_only") \
    X(str_playlist_type, "playlist_type") \
    X(str_media_sequence, "media_sequence") \
    X(str_targetduration, "targetduration") \
    X(str_discontinuity_sequence, "discontinuity_sequence") \
    X(str_version, "version") \
    X(str_allow_cache, "allow_cache") \
    X(str_start, "start") \
    X(str_server_control, "server_control") \
    X(str_preload_hint, "preload_hint") \
    X(str_content_steering, "content_steering") \
    X(str_stream_info, "stream_info") \
    X(str_parts, "parts") \
    X(str_iframe_stream_info, "iframe_stream_info") \
    X(str_image_stream_info, "image_stream_info") \
    /* Attribute schema keys */ \
    X(str_codecs, "codecs") \
    X(str_audio, "audio") \
    X(str_video, "video") \
    X(str_video_range, "video_range") \
    X(str_subtitles, "subtitles") \
    X(str_pathway_id, "pathway_id") \
    X(str_stable_variant_id, "stable_variant_id") \
    X(str_program_id, "program_id") \
    X(str_bandwidth, "bandwidth") \
    X(str_average_bandwidth, "average_bandwidth") \
    X(str_frame_rate, "frame_rate") \
    X(str_hdcp_level, "hdcp_level") \
    X(str_group_id, "group_id") \
    X(str_language, "language") \
    X(str_assoc_language, "assoc_language") \
    X(str_name, "name") \
    X(str_instream_id, "instream_id") \
    X(str_characteristics, "characteristics") \
    X(str_channels, "channels") \
    X(str_stable_rendition_id, "stable_rendition_id") \
    X(str_thumbnails, "thumbnails") \
    X(str_image, "image") \
    X(str_independent, "independent") \
    X(str_last_msn, "last_msn") \
    X(str_last_part, "last_part") \
    X(str_recently_removed_dateranges, "recently_removed_dateranges") \
    X(str_skipped_segments, "skipped_segments") \
    X(str_can_block_reload, "can_block_reload") \
    X(str_hold_back, "hold_back") \
    X(str_part_hold_back, "part_hold_back") \
    X(str_can_skip_until, "can_skip_until") \
    X(str_can_skip_dateranges, "can_skip_dateranges") \
    X(str_part_target, "part_target") \
    X(str_type, "type") \
    X(str_byterange_start, "byterange_start") \
    X(str_byterange_length, "byterange_length") \
    X(str_id, "id") \
    X(str_class, "class") \
    X(str_start_date, "start_date") \
    X(str_end_date, "end_date") \
    X(str_planned_duration, "planned_duration") \
    X(str_end_on_next, "end_on_next") \
    X(str_scte35_cmd, "scte35_cmd") \
    X(str_scte35_out, "scte35_out") \
    X(str_scte35_in, "scte35_in") \
    X(str_data_id, "data_id") \
    X(str_value, "value") \
    X(str_server_uri, "server_uri") \
    X(str_time_offset, "time_offset") \
    X(str_resolution, "resolution") \
    X(str_layout, "layout") \
    X(str_elapsedtime, "elapsedtime") \
    X(str_cue, "cue") \
    X(str_method, "method") \
    X(str_iv, "iv") \
    X(str_keyformat, "keyformat") \
    X(str_keyformatversions, "keyformatversions")

/* Module state (PEP 573 / subinterpreter safe). */
typedef struct {
    PyObject *ParseError;
    PyObject *datetime_cls;
    PyObject *timedelta_cls;
    PyObject *fromisoformat_meth;
    PyObject *int_zero;
    PyObject *float_zero;
    PyObject *segment_template;
    #define DECLARE_INTERNED(name, str) PyObject *name;
    INTERNED_STRINGS(DECLARE_INTERNED)
    #undef DECLARE_INTERNED
} m3u8_state;

/* Segment-level boolean flag bits in ParseContext.seg_flags */
#define SEG_FLAG_CUE_IN                     (1u << 0)
#define SEG_FLAG_CUE_OUT                    (1u << 1)
#define SEG_FLAG_CUE_OUT_START              (1u << 2)
#define SEG_FLAG_CUE_OUT_EXPLICIT_DURATION  (1u << 3)
#define SEG_FLAG_DISCONTINUITY              (1u << 4)
#define SEG_FLAG_GAP                        (1u << 5)

typedef struct {
    double secs;
    PyObject *td;  /* owned ref, NULL if slot unused */
} TimedeltaCacheEntry;

/*
 * ParseContext - holds all per-parse() state in C fields.
 *
 * Invariant for pending segment:
 *   Either ctx->segment != NULL (and ctx->has_extinf == 0), OR
 *   ctx->segment == NULL (and ctx->has_extinf may be 0 or 1).
 *
 * The Python `state` dict is only allocated and synchronized when
 * `custom_tags_parser` is callable.
 */
typedef struct {
    m3u8_state *mod_state;          /* Borrowed */
    PyObject *data;                 /* Owned by m3u8_parse */
    PyObject *state;                /* Owned, lazy (NULL unless custom_tags_parser) */
    PyObject *segments_list;        /* Borrowed ref to data["segments"] */
    PyObject *keys_list;            /* Borrowed ref to data["keys"] */
    int keys_has_none;              /* 1 once Py_None is known present in data["keys"] */
    int strict;
    int lineno;

    int expect_segment;
    int expect_playlist;
    unsigned int seg_flags;

    /* Deferred #EXTINF fields (used when segment == NULL) */
    int has_extinf;
    double extinf_duration;
    PyObject *extinf_duration_obj;  /* Owned ref */
    const char *extinf_title_ptr;   /* Points into pinned content buffer */
    size_t extinf_title_len;

    /* Pending segment dict (only created early for byterange/bitrate/part/custom) */
    PyObject *segment;              /* Owned ref or NULL */

    /* Parser state objects (all owned refs, NULL when unset) */
    PyObject *program_date_time;
    PyObject *current_program_date_time;
    int has_pending_pdt_add;
    double pending_pdt_secs;

    PyObject *current_key;
    PyObject *current_segment_map;
    PyObject *current_cue_out_scte35;
    PyObject *current_cue_out_oatcls_scte35;
    PyObject *current_cue_out_duration;
    PyObject *current_cue_out_elapsedtime;
    PyObject *asset_metadata;
    PyObject *dateranges;
    PyObject *blackout;
    PyObject *stream_info;

    /* Per-parse fast-path caches */
    char last_dur_buf[32];
    size_t last_dur_len;
    double last_dur_val;
    PyObject *last_dur_obj;         /* Owned ref */

    TimedeltaCacheEntry td_cache[2];
    int td_cache_next;

    const char *last_key_val_ptr;   /* Points into pinned content buffer */
    size_t last_key_val_len;
    PyObject *last_key_dict;        /* Owned ref */
} ParseContext;

static inline int
dict_set_interned(PyObject *dict, PyObject *interned_key, PyObject *value)
{
    return PyDict_SetItem(dict, interned_key, value);
}

static inline PyObject *
dict_get_interned(PyObject *dict, PyObject *interned_key)
{
    return PyDict_GetItemWithError(dict, interned_key);
}

static inline int
dict_del_if_present(PyObject *dict, PyObject *interned_key)
{
    /* Probe first: in sync_ctx_to_state_dict most optional keys are absent, so
     * avoiding PyDict_DelItem on misses avoids ~15 KeyError allocations/line. */
    PyObject *existing = PyDict_GetItemWithError(dict, interned_key);
    if (existing == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    if (PyDict_DelItem(dict, interned_key) < 0) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            return 0;
        }
        return -1;
    }
    return 0;
}

static inline int
append_to_list(PyObject *list, PyObject *item, const char *err_msg)
{
    if (list == NULL || !PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, err_msg);
        return -1;
    }
    return PyList_Append(list, item);
}

static inline int
append_to_data_list(ParseContext *ctx, PyObject *list_key, PyObject *item,
                    const char *err_msg)
{
    PyObject *list = dict_get_interned(ctx->data, list_key);
    if (list == NULL && PyErr_Occurred()) {
        return -1;
    }
    return append_to_list(list, item, err_msg);
}

static inline m3u8_state *
get_m3u8_state(PyObject *module)
{
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (m3u8_state *)state;
}

static void
parse_context_clear(ParseContext *ctx)
{
    Py_CLEAR(ctx->state);
    Py_CLEAR(ctx->extinf_duration_obj);
    Py_CLEAR(ctx->segment);
    Py_CLEAR(ctx->program_date_time);
    Py_CLEAR(ctx->current_program_date_time);
    Py_CLEAR(ctx->current_key);
    Py_CLEAR(ctx->current_segment_map);
    Py_CLEAR(ctx->current_cue_out_scte35);
    Py_CLEAR(ctx->current_cue_out_oatcls_scte35);
    Py_CLEAR(ctx->current_cue_out_duration);
    Py_CLEAR(ctx->current_cue_out_elapsedtime);
    Py_CLEAR(ctx->asset_metadata);
    Py_CLEAR(ctx->dateranges);
    Py_CLEAR(ctx->blackout);
    Py_CLEAR(ctx->stream_info);
    Py_CLEAR(ctx->last_dur_obj);
    Py_CLEAR(ctx->td_cache[0].td);
    Py_CLEAR(ctx->td_cache[1].td);
    Py_CLEAR(ctx->last_key_dict);
}

/*
 * Initialize datetime-related cached objects and immutable segment_template.
 */
static int
init_cached_objects(m3u8_state *state)
{
    PyObject *datetime_mod = PyImport_ImportModule("datetime");
    if (datetime_mod == NULL) {
        return -1;
    }

    state->datetime_cls = PyObject_GetAttrString(datetime_mod, "datetime");
    state->timedelta_cls = PyObject_GetAttrString(datetime_mod, "timedelta");
    if (state->datetime_cls != NULL) {
        state->fromisoformat_meth = PyObject_GetAttrString(
            state->datetime_cls, "fromisoformat");
    }
    Py_DECREF(datetime_mod);

    if (state->datetime_cls == NULL ||
        state->timedelta_cls == NULL ||
        state->fromisoformat_meth == NULL)
    {
        return -1;
    }

    state->int_zero = PyLong_FromLong(0);
    state->float_zero = PyFloat_FromDouble(0.0);
    if (state->int_zero == NULL || state->float_zero == NULL) {
        return -1;
    }

    /*
     * Pre-populate the 16 keys present on every complete segment dict.
     * CPython sizes this dict to 32 (usable capacity 21), so PyDict_Copy
     * allocates the final table in one shot with zero resizes even when
     * optional keys (program_date_time, current_program_date_time, key,
     * init_section) are inserted.
     */
    PyObject *tmpl = PyDict_New();
    if (tmpl == NULL) {
        return -1;
    }
    if (dict_set_interned(tmpl, state->str_duration, state->float_zero) < 0 ||
        dict_set_interned(tmpl, state->str_title, state->str_empty) < 0 ||
        dict_set_interned(tmpl, state->str_uri, state->str_empty) < 0 ||
        dict_set_interned(tmpl, state->str_cue_in, Py_False) < 0 ||
        dict_set_interned(tmpl, state->str_cue_out, Py_False) < 0 ||
        dict_set_interned(tmpl, state->str_cue_out_start, Py_False) < 0 ||
        dict_set_interned(tmpl, state->str_cue_out_explicitly_duration, Py_False) < 0 ||
        dict_set_interned(tmpl, state->str_scte35, Py_None) < 0 ||
        dict_set_interned(tmpl, state->str_oatcls_scte35, Py_None) < 0 ||
        dict_set_interned(tmpl, state->str_scte35_duration, Py_None) < 0 ||
        dict_set_interned(tmpl, state->str_scte35_elapsedtime, Py_None) < 0 ||
        dict_set_interned(tmpl, state->str_asset_metadata, Py_None) < 0 ||
        dict_set_interned(tmpl, state->str_discontinuity, Py_False) < 0 ||
        dict_set_interned(tmpl, state->str_dateranges, Py_None) < 0 ||
        dict_set_interned(tmpl, state->str_gap_tag, Py_None) < 0 ||
        dict_set_interned(tmpl, state->str_blackout, Py_None) < 0)
    {
        Py_DECREF(tmpl);
        return -1;
    }
    state->segment_template = tmpl;
    return 0;
}

static int
init_interned_strings(m3u8_state *state)
{
    #define INIT_INTERNED(name, str) \
        state->name = PyUnicode_InternFromString(str); \
        if (state->name == NULL) return -1;
    INTERNED_STRINGS(INIT_INTERNED)
    #undef INIT_INTERNED
    return 0;
}

static void
raise_parse_error(m3u8_state *state, int lineno, const char *line, size_t line_len)
{
    PyObject *py_lineno = PyLong_FromLong(lineno);
    if (py_lineno == NULL) {
        return;
    }

    PyObject *py_line = PyUnicode_FromStringAndSize(line, (Py_ssize_t)line_len);
    if (py_line == NULL) {
        Py_DECREF(py_lineno);
        return;
    }

    PyObject *exc = PyObject_CallFunctionObjArgs(state->ParseError, py_lineno, py_line, NULL);
    Py_DECREF(py_lineno);
    Py_DECREF(py_line);

    if (exc != NULL) {
        PyErr_SetObject(state->ParseError, exc);
        Py_DECREF(exc);
    }
}

/*
 * Add seconds to a datetime object using a 2-slot per-parse timedelta cache
 * and PyObject_CallFunctionObjArgs (vectorcall-backed in CPython 3.11+).
 */
static PyObject *
datetime_add_seconds(ParseContext *ctx, PyObject *dt, double secs)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *delta = NULL;

    /* Cache lookup (note: NaN != NaN so NaN never falsely hits) */
    if (ctx->td_cache[0].td != NULL && ctx->td_cache[0].secs == secs) {
        delta = ctx->td_cache[0].td;
    } else if (ctx->td_cache[1].td != NULL && ctx->td_cache[1].secs == secs) {
        delta = ctx->td_cache[1].td;
    }

    if (delta != NULL) {
        return PyNumber_Add(dt, delta);
    }

    PyObject *py_secs = PyFloat_FromDouble(secs);
    if (py_secs == NULL) {
        return NULL;
    }
    PyObject *new_td = PyObject_CallFunctionObjArgs(
        ms->timedelta_cls, ms->int_zero, py_secs, NULL);
    Py_DECREF(py_secs);
    if (new_td == NULL) {
        return NULL;
    }

    PyObject *new_dt = PyNumber_Add(dt, new_td);
    if (new_dt == NULL) {
        Py_DECREF(new_td);
        return NULL;
    }

    /* Store valid timedelta in 2-slot ring cache */
    int slot = ctx->td_cache_next;
    ctx->td_cache_next = 1 - slot;
    ctx->td_cache[slot].secs = secs;
    pyobj_replace(&ctx->td_cache[slot].td, new_td);

    return new_dt;
}

/*
 * Materialize any deferred current_program_date_time addition.
 */
static inline int
flush_pending_pdt(ParseContext *ctx)
{
    if (!ctx->has_pending_pdt_add) {
        return 0;
    }
    ctx->has_pending_pdt_add = 0;
    if (ctx->current_program_date_time == NULL) {
        return 0;
    }
    PyObject *new_pdt = datetime_add_seconds(
        ctx, ctx->current_program_date_time, ctx->pending_pdt_secs);
    if (new_pdt == NULL) {
        return -1;
    }
    pyobj_replace(&ctx->current_program_date_time, new_pdt);
    return 0;
}

/*
 * Materialize ctx->segment dict if not already allocated, flushing any
 * deferred #EXTINF duration/title into it.
 * Returns borrowed ref to ctx->segment or NULL on error.
 */
static PyObject *
materialize_segment(ParseContext *ctx)
{
    if (ctx->segment != NULL) {
        return ctx->segment;
    }
    m3u8_state *ms = ctx->mod_state;
    PyObject *seg = PyDict_New();
    if (seg == NULL) {
        return NULL;
    }
    if (ctx->has_extinf) {
        if (dict_set_interned(seg, ms->str_duration, ctx->extinf_duration_obj) < 0) {
            Py_DECREF(seg);
            return NULL;
        }
        PyObject *py_title;
        if (ctx->extinf_title_len == 0) {
            py_title = Py_NewRef(ms->str_empty);
        } else {
            py_title = PyUnicode_FromStringAndSize(
                ctx->extinf_title_ptr, (Py_ssize_t)ctx->extinf_title_len);
            if (py_title == NULL) {
                Py_DECREF(seg);
                return NULL;
            }
        }
        int rc = dict_set_interned(seg, ms->str_title, py_title);
        Py_DECREF(py_title);
        if (rc < 0) {
            Py_DECREF(seg);
            return NULL;
        }
        ctx->has_extinf = 0;
        Py_CLEAR(ctx->extinf_duration_obj);
        ctx->extinf_title_ptr = NULL;
        ctx->extinf_title_len = 0;
    }
    ctx->segment = seg;
    return seg;
}

/*
 * Sync ParseContext C fields TO Python state dict before calling custom_tags_parser.
 */
static int
sync_ctx_to_state_dict(ParseContext *ctx)
{
    m3u8_state *ms = ctx->mod_state;

    if (flush_pending_pdt(ctx) < 0) {
        return -1;
    }
    if (ctx->has_extinf && materialize_segment(ctx) == NULL) {
        return -1;
    }

    if (ctx->state == NULL) {
        ctx->state = PyDict_New();
        if (ctx->state == NULL) {
            return -1;
        }
    }
    PyObject *st = ctx->state;

    if (dict_set_interned(st, ms->str_expect_segment,
                          ctx->expect_segment ? Py_True : Py_False) < 0 ||
        dict_set_interned(st, ms->str_expect_playlist,
                          ctx->expect_playlist ? Py_True : Py_False) < 0 ||
        dict_set_interned(st, ms->str_current_key,
                          ctx->current_key ? ctx->current_key : Py_None) < 0 ||
        dict_set_interned(st, ms->str_current_segment_map,
                          ctx->current_segment_map ? ctx->current_segment_map : Py_None) < 0)
    {
        return -1;
    }

    #define SYNC_FLAG_TO_DICT(bit, key_field) \
        do { \
            if (ctx->seg_flags & (bit)) { \
                if (dict_set_interned(st, ms->key_field, Py_True) < 0) return -1; \
            } else { \
                if (dict_del_if_present(st, ms->key_field) < 0) return -1; \
            } \
        } while (0)

    SYNC_FLAG_TO_DICT(SEG_FLAG_CUE_IN, str_cue_in);
    SYNC_FLAG_TO_DICT(SEG_FLAG_CUE_OUT, str_cue_out);
    SYNC_FLAG_TO_DICT(SEG_FLAG_CUE_OUT_START, str_cue_out_start);
    SYNC_FLAG_TO_DICT(SEG_FLAG_CUE_OUT_EXPLICIT_DURATION, str_cue_out_explicitly_duration);
    SYNC_FLAG_TO_DICT(SEG_FLAG_DISCONTINUITY, str_discontinuity);
    SYNC_FLAG_TO_DICT(SEG_FLAG_GAP, str_gap);
    #undef SYNC_FLAG_TO_DICT

    #define SYNC_OBJ_TO_DICT(field, key_field) \
        do { \
            if (ctx->field != NULL) { \
                if (dict_set_interned(st, ms->key_field, ctx->field) < 0) return -1; \
            } else { \
                if (dict_del_if_present(st, ms->key_field) < 0) return -1; \
            } \
        } while (0)

    SYNC_OBJ_TO_DICT(segment, str_segment);
    SYNC_OBJ_TO_DICT(program_date_time, str_program_date_time);
    SYNC_OBJ_TO_DICT(current_program_date_time, str_current_program_date_time);
    SYNC_OBJ_TO_DICT(current_cue_out_scte35, str_current_cue_out_scte35);
    SYNC_OBJ_TO_DICT(current_cue_out_oatcls_scte35, str_current_cue_out_oatcls_scte35);
    SYNC_OBJ_TO_DICT(current_cue_out_duration, str_current_cue_out_duration);
    SYNC_OBJ_TO_DICT(current_cue_out_elapsedtime, str_current_cue_out_elapsedtime);
    SYNC_OBJ_TO_DICT(asset_metadata, str_asset_metadata);
    SYNC_OBJ_TO_DICT(dateranges, str_dateranges);
    SYNC_OBJ_TO_DICT(blackout, str_blackout);
    SYNC_OBJ_TO_DICT(stream_info, str_stream_info);
    #undef SYNC_OBJ_TO_DICT

    return 0;
}

/*
 * Sync Python state dict FROM custom_tags_parser back into ParseContext C fields.
 */
static int
sync_state_dict_to_ctx(ParseContext *ctx)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *st = ctx->state;
    PyObject *val;
    int truth;

    val = dict_get_interned(st, ms->str_expect_segment);
    if (val != NULL) {
        truth = PyObject_IsTrue(val);
        if (truth < 0) return -1;
        ctx->expect_segment = truth;
    } else if (PyErr_Occurred()) {
        return -1;
    } else {
        ctx->expect_segment = 0;
    }

    val = dict_get_interned(st, ms->str_expect_playlist);
    if (val != NULL) {
        truth = PyObject_IsTrue(val);
        if (truth < 0) return -1;
        ctx->expect_playlist = truth;
    } else if (PyErr_Occurred()) {
        return -1;
    } else {
        ctx->expect_playlist = 0;
    }

    ctx->seg_flags = 0;
    #define SYNC_FLAG_FROM_DICT(bit, key_field) \
        do { \
            val = dict_get_interned(st, ms->key_field); \
            if (val != NULL) { \
                truth = PyObject_IsTrue(val); \
                if (truth < 0) return -1; \
                if (truth) ctx->seg_flags |= (bit); \
            } else if (PyErr_Occurred()) { \
                return -1; \
            } \
        } while (0)

    SYNC_FLAG_FROM_DICT(SEG_FLAG_CUE_IN, str_cue_in);
    SYNC_FLAG_FROM_DICT(SEG_FLAG_CUE_OUT, str_cue_out);
    SYNC_FLAG_FROM_DICT(SEG_FLAG_CUE_OUT_START, str_cue_out_start);
    SYNC_FLAG_FROM_DICT(SEG_FLAG_CUE_OUT_EXPLICIT_DURATION, str_cue_out_explicitly_duration);
    SYNC_FLAG_FROM_DICT(SEG_FLAG_DISCONTINUITY, str_discontinuity);
    SYNC_FLAG_FROM_DICT(SEG_FLAG_GAP, str_gap);
    #undef SYNC_FLAG_FROM_DICT

    #define SYNC_OBJ_FROM_DICT(field, key_field, none_is_null) \
        do { \
            val = dict_get_interned(st, ms->key_field); \
            if (val == NULL) { \
                if (PyErr_Occurred()) return -1; \
                pyobj_replace(&ctx->field, NULL); \
            } else if ((none_is_null) && val == Py_None) { \
                pyobj_replace(&ctx->field, NULL); \
            } else { \
                pyobj_replace(&ctx->field, Py_NewRef(val)); \
            } \
        } while (0)

    SYNC_OBJ_FROM_DICT(segment, str_segment, 0);
    if (ctx->segment != NULL && !PyDict_Check(ctx->segment)) {
        PyErr_SetString(PyExc_TypeError, "state['segment'] must be a dict");
        return -1;
    }
    SYNC_OBJ_FROM_DICT(program_date_time, str_program_date_time, 1);
    SYNC_OBJ_FROM_DICT(current_program_date_time, str_current_program_date_time, 1);
    SYNC_OBJ_FROM_DICT(current_key, str_current_key, 1);
    SYNC_OBJ_FROM_DICT(current_segment_map, str_current_segment_map, 1);
    SYNC_OBJ_FROM_DICT(current_cue_out_scte35, str_current_cue_out_scte35, 0);
    SYNC_OBJ_FROM_DICT(current_cue_out_oatcls_scte35, str_current_cue_out_oatcls_scte35, 0);
    SYNC_OBJ_FROM_DICT(current_cue_out_duration, str_current_cue_out_duration, 0);
    SYNC_OBJ_FROM_DICT(current_cue_out_elapsedtime, str_current_cue_out_elapsedtime, 0);
    SYNC_OBJ_FROM_DICT(asset_metadata, str_asset_metadata, 0);
    SYNC_OBJ_FROM_DICT(dateranges, str_dateranges, 0);
    SYNC_OBJ_FROM_DICT(blackout, str_blackout, 0);
    SYNC_OBJ_FROM_DICT(stream_info, str_stream_info, 0);
    #undef SYNC_OBJ_FROM_DICT

    /* Invalidate cached data pointers/caches in case callback mutated data */
    ctx->segments_list = dict_get_interned(ctx->data, ms->str_segments);
    if (ctx->segments_list == NULL && PyErr_Occurred()) return -1;
    ctx->keys_list = dict_get_interned(ctx->data, ms->str_keys);
    if (ctx->keys_list == NULL && PyErr_Occurred()) return -1;
    ctx->keys_has_none = 0;
    ctx->last_key_val_ptr = NULL;
    ctx->last_key_val_len = 0;
    ctx->has_pending_pdt_add = 0;
    return 0;
}

/* Build list matching Python's content.strip().splitlines() (preserves internal blanks). */
static PyObject *
build_stripped_splitlines(const char *trimmed, size_t trimmed_len)
{
    const unsigned char *p = (const unsigned char *)trimmed;
    const unsigned char *end = p + trimmed_len;

    PyObject *lines = PyList_New(0);
    if (lines == NULL) {
        return NULL;
    }

    const unsigned char *line_start = p;
    while (p < end) {
        if (*p == '\n' || *p == '\r') {
            PyObject *line = PyUnicode_FromStringAndSize(
                (const char *)line_start, (Py_ssize_t)(p - line_start));
            if (line == NULL) {
                Py_DECREF(lines);
                return NULL;
            }
            if (PyList_Append(lines, line) < 0) {
                Py_DECREF(line);
                Py_DECREF(lines);
                return NULL;
            }
            Py_DECREF(line);

            if (*p == '\r' && (p + 1) < end && *(p + 1) == '\n') {
                p++;
            }
            p++;
            line_start = p;
            continue;
        }
        p++;
    }

    PyObject *line = PyUnicode_FromStringAndSize(
        (const char *)line_start, (Py_ssize_t)(end - line_start));
    if (line == NULL) {
        Py_DECREF(lines);
        return NULL;
    }
    if (PyList_Append(lines, line) < 0) {
        Py_DECREF(line);
        Py_DECREF(lines);
        return NULL;
    }
    Py_DECREF(line);

    return lines;
}

static int
init_list_fields(PyObject *data, PyObject **keys, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        PyObject *list = PyList_New(0);
        if (list == NULL) {
            return -1;
        }
        if (dict_set_interned(data, keys[i], list) < 0) {
            Py_DECREF(list);
            return -1;
        }
        Py_DECREF(list);
    }
    return 0;
}

static int
init_dict_fields(PyObject *data, PyObject **keys, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        PyObject *dict = PyDict_New();
        if (dict == NULL) {
            return -1;
        }
        if (dict_set_interned(data, keys[i], dict) < 0) {
            Py_DECREF(dict);
            return -1;
        }
        Py_DECREF(dict);
    }
    return 0;
}

static PyObject *
init_parse_data(m3u8_state *ms)
{
    PyObject *data = PyDict_New();
    if (data == NULL) {
        return NULL;
    }

    if (dict_set_interned(data, ms->str_media_sequence, ms->int_zero) < 0 ||
        dict_set_interned(data, ms->str_is_variant, Py_False) < 0 ||
        dict_set_interned(data, ms->str_is_endlist, Py_False) < 0 ||
        dict_set_interned(data, ms->str_is_i_frames_only, Py_False) < 0 ||
        dict_set_interned(data, ms->str_is_independent_segments, Py_False) < 0 ||
        dict_set_interned(data, ms->str_is_images_only, Py_False) < 0 ||
        dict_set_interned(data, ms->str_playlist_type, Py_None) < 0)
    {
        goto fail;
    }

    PyObject *list_keys[] = {
        ms->str_playlists,
        ms->str_segments,
        ms->str_iframe_playlists,
        ms->str_image_playlists,
        ms->str_tiles,
        ms->str_media,
        ms->str_keys,
        ms->str_rendition_reports,
        ms->str_session_data,
        ms->str_session_keys,
        ms->str_segment_map,
    };
    if (init_list_fields(data, list_keys, sizeof(list_keys) / sizeof(list_keys[0])) < 0) {
        goto fail;
    }

    PyObject *dict_keys[] = {
        ms->str_skip,
        ms->str_part_inf,
    };
    if (init_dict_fields(data, dict_keys, sizeof(dict_keys) / sizeof(dict_keys[0])) < 0) {
        goto fail;
    }

    return data;

fail:
    Py_DECREF(data);
    return NULL;
}

/*
 * Create normalized Python string from raw buffer:
 * strip leading/trailing whitespace, lowercase ASCII, replace '-' with '_'.
 */
static PyObject *
create_normalized_key(const char *s, size_t len)
{
    while (len > 0 && ascii_isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && ascii_isspace((unsigned char)s[len - 1])) {
        len--;
    }

    char stack_buf[64];
    char *buf = stack_buf;
    int use_heap = (len >= sizeof(stack_buf));
    if (use_heap) {
        buf = PyMem_Malloc(len + 1);
        if (buf == NULL) {
            return PyErr_NoMemory();
        }
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '-') {
            c = '_';
        } else {
            c = ascii_tolower(c);
        }
        buf[i] = (char)c;
    }

    PyObject *res = PyUnicode_FromStringAndSize(buf, (Py_ssize_t)len);
    if (use_heap) {
        PyMem_Free(buf);
    }
    return res;
}

/*
 * Create lowercased, stripped Python string (for PLAYLIST-TYPE / ALLOW-CACHE).
 * Preserves '-' (matching Python's value.strip().lower()).
 */
static PyObject *
create_lower_stripped(const char *s, size_t len)
{
    while (len > 0 && ascii_isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && ascii_isspace((unsigned char)s[len - 1])) {
        len--;
    }

    char stack_buf[64];
    char *buf = stack_buf;
    int use_heap = (len >= sizeof(stack_buf));
    if (use_heap) {
        buf = PyMem_Malloc(len + 1);
        if (buf == NULL) {
            return PyErr_NoMemory();
        }
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)ascii_tolower((unsigned char)s[i]);
    }

    PyObject *res = PyUnicode_FromStringAndSize(buf, (Py_ssize_t)len);
    if (use_heap) {
        PyMem_Free(buf);
    }
    return res;
}

/* Attribute type definitions for schema-aware attribute parsing */
typedef enum {
    ATTR_STRING,
    ATTR_INT,
    ATTR_FLOAT,
    ATTR_QUOTED_STRING,
    ATTR_BANDWIDTH
} AttrType;

typedef struct {
    const char *name;
    size_t name_len;
    size_t key_offset;   /* offsetof(m3u8_state, str_...) */
    AttrType type;
} AttrParser;

#define ATTR_DEF(name_str, field, attr_type) \
    {name_str, sizeof(name_str) - 1, offsetof(m3u8_state, field), attr_type}

/*
 * Schema-aware zero-copy attribute list parser.
 *
 * Uses pre-interned PyObject* keys from m3u8_state on schema hit and converts
 * values directly to their target Python types. When unquote_all != 0, strips
 * surrounding quotes from all values in a single pass (for EXT-X-KEY / SESSION-KEY).
 */
static PyObject *
parse_attributes_with_schema(m3u8_state *ms, const char *start, const char *end,
                             const AttrParser *parsers, size_t num_parsers,
                             int unquote_all)
{
    PyObject *attrs = PyDict_New();
    if (attrs == NULL) {
        return NULL;
    }

    const char *p = start;
    while (p < end) {
        while (p < end && (ascii_isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (p >= end) {
            break;
        }

        const char *key_start = p;
        while (p < end && *p != '=' && *p != ',') {
            p++;
        }
        const char *key_end = p;

        if (p < end && *p == '=') {
            p++;  /* Skip '=' */

            /* Trim whitespace around key for schema lookup and normalization */
            const char *tk_start = key_start;
            const char *tk_end = key_end;
            while (tk_start < tk_end && ascii_isspace((unsigned char)*tk_start)) {
                tk_start++;
            }
            while (tk_end > tk_start && ascii_isspace((unsigned char)*(tk_end - 1))) {
                tk_end--;
            }
            size_t tk_len = (size_t)(tk_end - tk_start);

            AttrType type = unquote_all ? ATTR_QUOTED_STRING : ATTR_STRING;
            PyObject *py_key = NULL;
            if (parsers != NULL) {
                for (size_t i = 0; i < num_parsers; i++) {
                    if (parsers[i].name_len == tk_len &&
                        buffer_matches_key(tk_start, tk_len, parsers[i].name))
                    {
                        type = unquote_all ? ATTR_QUOTED_STRING : parsers[i].type;
                        py_key = Py_NewRef(*(PyObject **)((char *)ms + parsers[i].key_offset));
                        break;
                    }
                }
            }
            if (py_key == NULL) {
                py_key = create_normalized_key(tk_start, tk_len);
                if (py_key == NULL) {
                    Py_DECREF(attrs);
                    return NULL;
                }
            }

            PyObject *py_val = NULL;
            if (p < end && (*p == '"' || *p == '\'')) {
                char quote = *p++;
                const char *full_start = p - 1;
                const char *val_start = p;
                while (p < end && *p != quote) {
                    p++;
                }
                const char *val_end = p;
                int has_closing_quote = (p < end && *p == quote);
                size_t val_len = (size_t)(val_end - val_start);
                if (has_closing_quote) {
                    p++;
                }

                if (type == ATTR_QUOTED_STRING) {
                    if (has_closing_quote) {
                        py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
                    } else {
                        py_val = PyUnicode_FromStringAndSize(
                            full_start, (Py_ssize_t)(val_end - full_start));
                    }
                } else if (type == ATTR_STRING) {
                    Py_ssize_t full_len = has_closing_quote
                        ? (Py_ssize_t)((val_end - full_start) + 1)
                        : (Py_ssize_t)(val_end - full_start);
                    py_val = PyUnicode_FromStringAndSize(full_start, full_len);
                } else if (type == ATTR_INT) {
                    py_val = parse_int_slice(val_start, val_len);
                    if (py_val == NULL) {
                        if (PyErr_Occurred()) {
                            Py_DECREF(py_key);
                            Py_DECREF(attrs);
                            return NULL;
                        }
                        py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
                    }
                } else if (type == ATTR_BANDWIDTH) {
                    double v;
                    if (parse_double_slice(val_start, val_len, &v) == 0 && isfinite(v)) {
                        py_val = PyLong_FromDouble(v);
                    } else {
                        py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
                    }
                } else if (type == ATTR_FLOAT) {
                    double v;
                    if (parse_double_slice(val_start, val_len, &v) == 0) {
                        py_val = PyFloat_FromDouble(v);
                    } else {
                        py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
                    }
                }
            } else {
                const char *val_start = p;
                while (p < end && *p != ',') {
                    p++;
                }
                const char *val_end = p;
                while (val_end > val_start && ascii_isspace((unsigned char)*(val_end - 1))) {
                    val_end--;
                }
                size_t val_len = (size_t)(val_end - val_start);

                if (type == ATTR_INT) {
                    py_val = parse_int_slice(val_start, val_len);
                    if (py_val == NULL) {
                        if (PyErr_Occurred()) {
                            Py_DECREF(py_key);
                            Py_DECREF(attrs);
                            return NULL;
                        }
                        py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
                    }
                } else if (type == ATTR_BANDWIDTH) {
                    double v;
                    if (parse_double_slice(val_start, val_len, &v) == 0 && isfinite(v)) {
                        py_val = PyLong_FromDouble(v);
                    } else {
                        py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
                    }
                } else if (type == ATTR_FLOAT) {
                    double v;
                    if (parse_double_slice(val_start, val_len, &v) == 0) {
                        py_val = PyFloat_FromDouble(v);
                    } else {
                        py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
                    }
                } else {
                    py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
                }
            }

            if (py_val == NULL) {
                Py_DECREF(py_key);
                Py_DECREF(attrs);
                return NULL;
            }
            if (PyDict_SetItem(attrs, py_key, py_val) < 0) {
                Py_DECREF(py_key);
                Py_DECREF(py_val);
                Py_DECREF(attrs);
                return NULL;
            }
            Py_DECREF(py_key);
            Py_DECREF(py_val);
        } else {
            /* Bare token without '=' -> stored under empty key "" (e.g. CUE-OUT-CONT:2.436/120) */
            const char *val_start = key_start;
            const char *val_end = key_end;
            while (val_end > val_start && ascii_isspace((unsigned char)*(val_end - 1))) {
                val_end--;
            }
            size_t val_len = (size_t)(val_end - val_start);
            if (unquote_all && val_len >= 2 &&
                (val_start[0] == '"' || val_start[0] == '\'') &&
                (val_end[-1] == '"' || val_end[-1] == '\''))
            {
                val_start++;
                val_len -= 2;
            }
            PyObject *py_val = PyUnicode_FromStringAndSize(val_start, (Py_ssize_t)val_len);
            if (py_val == NULL) {
                Py_DECREF(attrs);
                return NULL;
            }
            if (dict_set_interned(attrs, ms->str_empty, py_val) < 0) {
                Py_DECREF(py_val);
                Py_DECREF(attrs);
                return NULL;
            }
            Py_DECREF(py_val);
        }
    }

    return attrs;
}

/* Attribute parser schemas */
static const AttrParser stream_inf_parsers[] = {
    ATTR_DEF("codecs", str_codecs, ATTR_QUOTED_STRING),
    ATTR_DEF("audio", str_audio, ATTR_QUOTED_STRING),
    ATTR_DEF("video", str_video, ATTR_QUOTED_STRING),
    ATTR_DEF("video_range", str_video_range, ATTR_QUOTED_STRING),
    ATTR_DEF("subtitles", str_subtitles, ATTR_QUOTED_STRING),
    ATTR_DEF("pathway_id", str_pathway_id, ATTR_QUOTED_STRING),
    ATTR_DEF("stable_variant_id", str_stable_variant_id, ATTR_QUOTED_STRING),
    ATTR_DEF("program_id", str_program_id, ATTR_INT),
    ATTR_DEF("bandwidth", str_bandwidth, ATTR_BANDWIDTH),
    ATTR_DEF("average_bandwidth", str_average_bandwidth, ATTR_INT),
    ATTR_DEF("frame_rate", str_frame_rate, ATTR_FLOAT),
    ATTR_DEF("hdcp_level", str_hdcp_level, ATTR_STRING),
};
#define NUM_STREAM_INF_PARSERS (sizeof(stream_inf_parsers) / sizeof(stream_inf_parsers[0]))

static const AttrParser media_parsers[] = {
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("group_id", str_group_id, ATTR_QUOTED_STRING),
    ATTR_DEF("language", str_language, ATTR_QUOTED_STRING),
    ATTR_DEF("assoc_language", str_assoc_language, ATTR_QUOTED_STRING),
    ATTR_DEF("name", str_name, ATTR_QUOTED_STRING),
    ATTR_DEF("instream_id", str_instream_id, ATTR_QUOTED_STRING),
    ATTR_DEF("characteristics", str_characteristics, ATTR_QUOTED_STRING),
    ATTR_DEF("channels", str_channels, ATTR_QUOTED_STRING),
    ATTR_DEF("stable_rendition_id", str_stable_rendition_id, ATTR_QUOTED_STRING),
    ATTR_DEF("thumbnails", str_thumbnails, ATTR_QUOTED_STRING),
    ATTR_DEF("image", str_image, ATTR_QUOTED_STRING),
};
#define NUM_MEDIA_PARSERS (sizeof(media_parsers) / sizeof(media_parsers[0]))

static const AttrParser part_parsers[] = {
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("duration", str_duration, ATTR_FLOAT),
    ATTR_DEF("independent", str_independent, ATTR_STRING),
    ATTR_DEF("gap", str_gap, ATTR_STRING),
    ATTR_DEF("byterange", str_byterange, ATTR_STRING),
};
#define NUM_PART_PARSERS (sizeof(part_parsers) / sizeof(part_parsers[0]))

static const AttrParser rendition_report_parsers[] = {
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("last_msn", str_last_msn, ATTR_INT),
    ATTR_DEF("last_part", str_last_part, ATTR_INT),
};
#define NUM_RENDITION_REPORT_PARSERS (sizeof(rendition_report_parsers) / sizeof(rendition_report_parsers[0]))

static const AttrParser skip_parsers[] = {
    ATTR_DEF("recently_removed_dateranges", str_recently_removed_dateranges, ATTR_QUOTED_STRING),
    ATTR_DEF("skipped_segments", str_skipped_segments, ATTR_INT),
};
#define NUM_SKIP_PARSERS (sizeof(skip_parsers) / sizeof(skip_parsers[0]))

static const AttrParser server_control_parsers[] = {
    ATTR_DEF("can_block_reload", str_can_block_reload, ATTR_STRING),
    ATTR_DEF("hold_back", str_hold_back, ATTR_FLOAT),
    ATTR_DEF("part_hold_back", str_part_hold_back, ATTR_FLOAT),
    ATTR_DEF("can_skip_until", str_can_skip_until, ATTR_FLOAT),
    ATTR_DEF("can_skip_dateranges", str_can_skip_dateranges, ATTR_STRING),
};
#define NUM_SERVER_CONTROL_PARSERS (sizeof(server_control_parsers) / sizeof(server_control_parsers[0]))

static const AttrParser part_inf_parsers[] = {
    ATTR_DEF("part_target", str_part_target, ATTR_FLOAT),
};
#define NUM_PART_INF_PARSERS (sizeof(part_inf_parsers) / sizeof(part_inf_parsers[0]))

static const AttrParser preload_hint_parsers[] = {
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("type", str_type, ATTR_STRING),
    ATTR_DEF("byterange_start", str_byterange_start, ATTR_INT),
    ATTR_DEF("byterange_length", str_byterange_length, ATTR_INT),
};
#define NUM_PRELOAD_HINT_PARSERS (sizeof(preload_hint_parsers) / sizeof(preload_hint_parsers[0]))

static const AttrParser daterange_parsers[] = {
    ATTR_DEF("id", str_id, ATTR_QUOTED_STRING),
    ATTR_DEF("class", str_class, ATTR_QUOTED_STRING),
    ATTR_DEF("start_date", str_start_date, ATTR_QUOTED_STRING),
    ATTR_DEF("end_date", str_end_date, ATTR_QUOTED_STRING),
    ATTR_DEF("duration", str_duration, ATTR_FLOAT),
    ATTR_DEF("planned_duration", str_planned_duration, ATTR_FLOAT),
    ATTR_DEF("end_on_next", str_end_on_next, ATTR_STRING),
    ATTR_DEF("scte35_cmd", str_scte35_cmd, ATTR_STRING),
    ATTR_DEF("scte35_out", str_scte35_out, ATTR_STRING),
    ATTR_DEF("scte35_in", str_scte35_in, ATTR_STRING),
};
#define NUM_DATERANGE_PARSERS (sizeof(daterange_parsers) / sizeof(daterange_parsers[0]))

static const AttrParser session_data_parsers[] = {
    ATTR_DEF("data_id", str_data_id, ATTR_QUOTED_STRING),
    ATTR_DEF("value", str_value, ATTR_QUOTED_STRING),
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("language", str_language, ATTR_QUOTED_STRING),
};
#define NUM_SESSION_DATA_PARSERS (sizeof(session_data_parsers) / sizeof(session_data_parsers[0]))

static const AttrParser content_steering_parsers[] = {
    ATTR_DEF("server_uri", str_server_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("pathway_id", str_pathway_id, ATTR_QUOTED_STRING),
};
#define NUM_CONTENT_STEERING_PARSERS (sizeof(content_steering_parsers) / sizeof(content_steering_parsers[0]))

static const AttrParser x_map_parsers[] = {
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("byterange", str_byterange, ATTR_QUOTED_STRING),
};
#define NUM_X_MAP_PARSERS (sizeof(x_map_parsers) / sizeof(x_map_parsers[0]))

static const AttrParser start_parsers[] = {
    ATTR_DEF("time_offset", str_time_offset, ATTR_FLOAT),
};
#define NUM_START_PARSERS (sizeof(start_parsers) / sizeof(start_parsers[0]))

static const AttrParser tiles_parsers[] = {
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("resolution", str_resolution, ATTR_STRING),
    ATTR_DEF("layout", str_layout, ATTR_STRING),
    ATTR_DEF("duration", str_duration, ATTR_FLOAT),
};
#define NUM_TILES_PARSERS (sizeof(tiles_parsers) / sizeof(tiles_parsers[0]))

static const AttrParser image_stream_inf_parsers[] = {
    ATTR_DEF("codecs", str_codecs, ATTR_QUOTED_STRING),
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("pathway_id", str_pathway_id, ATTR_QUOTED_STRING),
    ATTR_DEF("stable_variant_id", str_stable_variant_id, ATTR_QUOTED_STRING),
    ATTR_DEF("program_id", str_program_id, ATTR_INT),
    ATTR_DEF("bandwidth", str_bandwidth, ATTR_INT),
    ATTR_DEF("average_bandwidth", str_average_bandwidth, ATTR_INT),
    ATTR_DEF("resolution", str_resolution, ATTR_STRING),
};
#define NUM_IMAGE_STREAM_INF_PARSERS (sizeof(image_stream_inf_parsers) / sizeof(image_stream_inf_parsers[0]))

static const AttrParser iframe_stream_inf_parsers[] = {
    ATTR_DEF("codecs", str_codecs, ATTR_QUOTED_STRING),
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("pathway_id", str_pathway_id, ATTR_QUOTED_STRING),
    ATTR_DEF("stable_variant_id", str_stable_variant_id, ATTR_QUOTED_STRING),
    ATTR_DEF("program_id", str_program_id, ATTR_INT),
    ATTR_DEF("bandwidth", str_bandwidth, ATTR_INT),
    ATTR_DEF("average_bandwidth", str_average_bandwidth, ATTR_INT),
    ATTR_DEF("hdcp_level", str_hdcp_level, ATTR_STRING),
};
#define NUM_IFRAME_STREAM_INF_PARSERS (sizeof(iframe_stream_inf_parsers) / sizeof(iframe_stream_inf_parsers[0]))

static const AttrParser cueout_cont_parsers[] = {
    ATTR_DEF("duration", str_duration, ATTR_QUOTED_STRING),
    ATTR_DEF("elapsedtime", str_elapsedtime, ATTR_QUOTED_STRING),
    ATTR_DEF("scte35", str_scte35, ATTR_QUOTED_STRING),
};
#define NUM_CUEOUT_CONT_PARSERS (sizeof(cueout_cont_parsers) / sizeof(cueout_cont_parsers[0]))

static const AttrParser cueout_parsers[] = {
    ATTR_DEF("cue", str_cue, ATTR_QUOTED_STRING),
    ATTR_DEF("duration", str_duration, ATTR_STRING),
};
#define NUM_CUEOUT_PARSERS (sizeof(cueout_parsers) / sizeof(cueout_parsers[0]))

static const AttrParser key_parsers[] = {
    ATTR_DEF("method", str_method, ATTR_QUOTED_STRING),
    ATTR_DEF("uri", str_uri, ATTR_QUOTED_STRING),
    ATTR_DEF("iv", str_iv, ATTR_QUOTED_STRING),
    ATTR_DEF("keyformat", str_keyformat, ATTR_QUOTED_STRING),
    ATTR_DEF("keyformatversions", str_keyformatversions, ATTR_QUOTED_STRING),
};
#define NUM_KEY_PARSERS (sizeof(key_parsers) / sizeof(key_parsers[0]))

/*
 * Ensure Py_None is in data["keys"] for unencrypted segments.
 */
static int
ensure_none_in_keys(ParseContext *ctx)
{
    PyObject *keys = ctx->keys_list;
    if (keys == NULL || !PyList_Check(keys)) {
        PyErr_SetString(PyExc_TypeError, "data['keys'] must be a list");
        return -1;
    }
    Py_ssize_t n = PyList_Size(keys);
    if (n < 0) {
        return -1;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        if (PyList_GetItem(keys, i) == Py_None) {
            ctx->keys_has_none = 1;
            return 0;
        }
    }
    if (PyList_Append(keys, Py_None) < 0) {
        return -1;
    }
    ctx->keys_has_none = 1;
    return 0;
}

/*
 * Parse #EXTINF:<duration>[,<title>]
 */
static int
handle_extinf(ParseContext *ctx, const char *line, size_t line_len,
              const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;
    const char *comma = memchr(val, ',', val_len);
    const char *dur_start = val;
    size_t dur_len;
    const char *title_start;
    size_t title_len;

    if (comma != NULL) {
        dur_len = (size_t)(comma - val);
        title_start = comma + 1;
        title_len = val_len - dur_len - 1;
    } else {
        if (ctx->strict) {
            raise_parse_error(ms, ctx->lineno, line, line_len);
            return -1;
        }
        dur_len = val_len;
        title_start = NULL;
        title_len = 0;
    }

    double duration;
    PyObject *py_duration;

    if (dur_len > 0 && dur_len == ctx->last_dur_len &&
        ctx->last_dur_obj != NULL &&
        memcmp(dur_start, ctx->last_dur_buf, dur_len) == 0)
    {
        duration = ctx->last_dur_val;
        py_duration = Py_NewRef(ctx->last_dur_obj);
    } else {
        if (parse_double_slice(dur_start, dur_len, &duration) < 0) {
            duration = 0.0;
        }
        py_duration = PyFloat_FromDouble(duration);
        if (py_duration == NULL) {
            return -1;
        }
        if (dur_len > 0 && dur_len <= sizeof(ctx->last_dur_buf)) {
            memcpy(ctx->last_dur_buf, dur_start, dur_len);
            ctx->last_dur_len = dur_len;
            ctx->last_dur_val = duration;
            pyobj_replace(&ctx->last_dur_obj, Py_NewRef(py_duration));
        }
    }

    if (ctx->segment != NULL) {
        /* Segment dict already exists (e.g. byterange/bitrate/part/custom parser) */
        if (dict_set_interned(ctx->segment, ms->str_duration, py_duration) < 0) {
            Py_DECREF(py_duration);
            return -1;
        }
        Py_DECREF(py_duration);

        PyObject *py_title;
        if (title_len == 0) {
            py_title = Py_NewRef(ms->str_empty);
        } else {
            py_title = PyUnicode_FromStringAndSize(title_start, (Py_ssize_t)title_len);
            if (py_title == NULL) {
                return -1;
            }
        }
        int rc = dict_set_interned(ctx->segment, ms->str_title, py_title);
        Py_DECREF(py_title);
        if (rc < 0) {
            return -1;
        }
    } else {
        /* Fast path: defer segment dict creation until URI line */
        ctx->has_extinf = 1;
        ctx->extinf_duration = duration;
        pyobj_replace(&ctx->extinf_duration_obj, py_duration);
        ctx->extinf_title_ptr = title_start;
        ctx->extinf_title_len = title_len;
    }

    ctx->expect_segment = 1;
    return 0;
}

/*
 * Parse a segment URI line and append the finalized segment dict to data["segments"].
 */
static int
parse_ts_chunk(ParseContext *ctx, const char *line, size_t line_len)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *segment;
    PyObject *uri = PyUnicode_FromStringAndSize(line, (Py_ssize_t)line_len);
    if (uri == NULL) {
        return -1;
    }

    /* Flush any deferred current_program_date_time addition before reading it */
    if (ctx->has_pending_pdt_add && flush_pending_pdt(ctx) < 0) {
        Py_DECREF(uri);
        return -1;
    }

    unsigned int flags = ctx->seg_flags;
    int cue_out_truth = (flags & SEG_FLAG_CUE_OUT) != 0;

    if (ctx->segment == NULL && ctx->has_extinf) {
        /*
         * Fast path: copy the 16-key pre-built segment template and overwrite
         * only varying/non-default keys.
         */
        segment = PyDict_Copy(ms->segment_template);
        if (segment == NULL) {
            Py_DECREF(uri);
            return -1;
        }

        double dur = ctx->extinf_duration;
        if (dict_set_interned(segment, ms->str_duration, ctx->extinf_duration_obj) < 0 ||
            dict_set_interned(segment, ms->str_uri, uri) < 0)
        {
            Py_DECREF(uri);
            Py_DECREF(segment);
            return -1;
        }
        Py_DECREF(uri);

        if (ctx->extinf_title_len > 0) {
            PyObject *py_title = PyUnicode_FromStringAndSize(
                ctx->extinf_title_ptr, (Py_ssize_t)ctx->extinf_title_len);
            if (py_title == NULL) {
                Py_DECREF(segment);
                return -1;
            }
            int rc = dict_set_interned(segment, ms->str_title, py_title);
            Py_DECREF(py_title);
            if (rc < 0) {
                Py_DECREF(segment);
                return -1;
            }
        }

        ctx->has_extinf = 0;
        Py_CLEAR(ctx->extinf_duration_obj);
        ctx->extinf_title_ptr = NULL;
        ctx->extinf_title_len = 0;

        /* Program date time */
        if (ctx->program_date_time != NULL) {
            if (dict_set_interned(segment, ms->str_program_date_time, ctx->program_date_time) < 0) {
                Py_DECREF(segment);
                return -1;
            }
            Py_CLEAR(ctx->program_date_time);
        }
        if (ctx->current_program_date_time != NULL) {
            if (dict_set_interned(segment, ms->str_current_program_date_time,
                                  ctx->current_program_date_time) < 0)
            {
                Py_DECREF(segment);
                return -1;
            }
            if (isfinite(dur) && fabs(dur) < 1e12) {
                ctx->has_pending_pdt_add = 1;
                ctx->pending_pdt_secs = dur;
            } else {
                PyObject *new_pdt = datetime_add_seconds(ctx, ctx->current_program_date_time, dur);
                if (new_pdt == NULL) {
                    Py_DECREF(segment);
                    return -1;
                }
                pyobj_replace(&ctx->current_program_date_time, new_pdt);
            }
        }

        /* Non-default boolean flags (template already has False/None defaults) */
        if (flags != 0) {
            if ((flags & SEG_FLAG_CUE_IN) &&
                dict_set_interned(segment, ms->str_cue_in, Py_True) < 0) goto fail_seg;
            if ((flags & SEG_FLAG_CUE_OUT) &&
                dict_set_interned(segment, ms->str_cue_out, Py_True) < 0) goto fail_seg;
            if ((flags & SEG_FLAG_CUE_OUT_START) &&
                dict_set_interned(segment, ms->str_cue_out_start, Py_True) < 0) goto fail_seg;
            if ((flags & SEG_FLAG_CUE_OUT_EXPLICIT_DURATION) &&
                dict_set_interned(segment, ms->str_cue_out_explicitly_duration, Py_True) < 0) goto fail_seg;
            if ((flags & SEG_FLAG_DISCONTINUITY) &&
                dict_set_interned(segment, ms->str_discontinuity, Py_True) < 0) goto fail_seg;
            if ((flags & SEG_FLAG_GAP) &&
                dict_set_interned(segment, ms->str_gap_tag, Py_True) < 0) goto fail_seg;
        }

        /* SCTE-35 / asset metadata (template already has None defaults) */
        if (ctx->current_cue_out_scte35 != NULL ||
            ctx->current_cue_out_oatcls_scte35 != NULL ||
            ctx->current_cue_out_duration != NULL ||
            ctx->current_cue_out_elapsedtime != NULL ||
            ctx->asset_metadata != NULL)
        {
            #define TRANSFER_SCTE_FAST(src_field, dst_key) \
                do { \
                    if (ctx->src_field != NULL) { \
                        if (dict_set_interned(segment, ms->dst_key, ctx->src_field) < 0) goto fail_seg; \
                        if (!cue_out_truth) { Py_CLEAR(ctx->src_field); } \
                    } \
                } while (0)
            TRANSFER_SCTE_FAST(current_cue_out_scte35, str_scte35);
            TRANSFER_SCTE_FAST(current_cue_out_oatcls_scte35, str_oatcls_scte35);
            TRANSFER_SCTE_FAST(current_cue_out_duration, str_scte35_duration);
            TRANSFER_SCTE_FAST(current_cue_out_elapsedtime, str_scte35_elapsedtime);
            TRANSFER_SCTE_FAST(asset_metadata, str_asset_metadata);
            #undef TRANSFER_SCTE_FAST
        }

        /* Key */
        if (ctx->current_key != NULL) {
            if (dict_set_interned(segment, ms->str_key, ctx->current_key) < 0) goto fail_seg;
        } else if (!ctx->keys_has_none) {
            if (ensure_none_in_keys(ctx) < 0) goto fail_seg;
        }

        /* Init section */
        if (ctx->current_segment_map != NULL) {
            int has_map = PyDict_Check(ctx->current_segment_map)
                ? (PyDict_Size(ctx->current_segment_map) > 0)
                : PyObject_IsTrue(ctx->current_segment_map);
            if (has_map < 0) goto fail_seg;
            if (has_map &&
                dict_set_interned(segment, ms->str_init_section, ctx->current_segment_map) < 0)
            {
                goto fail_seg;
            }
        }

        /* Dateranges and blackout (template already has None defaults) */
        if (ctx->dateranges != NULL) {
            if (dict_set_interned(segment, ms->str_dateranges, ctx->dateranges) < 0) goto fail_seg;
            Py_CLEAR(ctx->dateranges);
        }
        if (ctx->blackout != NULL) {
            if (dict_set_interned(segment, ms->str_blackout, ctx->blackout) < 0) goto fail_seg;
            Py_CLEAR(ctx->blackout);
        }
    } else {
        /*
         * General path: segment dict already exists (byterange, bitrate, parts,
         * or custom_tags_parser) or URI arrived without #EXTINF.
         */
        if (ctx->segment != NULL) {
            segment = ctx->segment;
            ctx->segment = NULL;
        } else {
            segment = PyDict_New();
            if (segment == NULL) {
                Py_DECREF(uri);
                return -1;
            }
        }

        if (dict_set_interned(segment, ms->str_uri, uri) < 0) {
            Py_DECREF(uri);
            Py_DECREF(segment);
            return -1;
        }
        Py_DECREF(uri);

        if (ctx->program_date_time != NULL) {
            if (dict_set_interned(segment, ms->str_program_date_time, ctx->program_date_time) < 0) {
                goto fail_seg;
            }
            Py_CLEAR(ctx->program_date_time);
        }
        if (ctx->current_program_date_time != NULL) {
            if (dict_set_interned(segment, ms->str_current_program_date_time,
                                  ctx->current_program_date_time) < 0)
            {
                goto fail_seg;
            }
            PyObject *duration_obj = dict_get_interned(segment, ms->str_duration);
            if (duration_obj == NULL && PyErr_Occurred()) {
                goto fail_seg;
            }
            if (duration_obj != NULL) {
                double secs = PyFloat_AsDouble(duration_obj);
                if (secs == -1.0 && PyErr_Occurred()) {
                    goto fail_seg;
                }
                if (isfinite(secs) && fabs(secs) < 1e12) {
                    ctx->has_pending_pdt_add = 1;
                    ctx->pending_pdt_secs = secs;
                } else {
                    PyObject *new_pdt = datetime_add_seconds(
                        ctx, ctx->current_program_date_time, secs);
                    if (new_pdt == NULL) {
                        goto fail_seg;
                    }
                    pyobj_replace(&ctx->current_program_date_time, new_pdt);
                }
            }
        }

        if (dict_set_interned(segment, ms->str_cue_in,
                              (flags & SEG_FLAG_CUE_IN) ? Py_True : Py_False) < 0 ||
            dict_set_interned(segment, ms->str_cue_out,
                              cue_out_truth ? Py_True : Py_False) < 0 ||
            dict_set_interned(segment, ms->str_cue_out_start,
                              (flags & SEG_FLAG_CUE_OUT_START) ? Py_True : Py_False) < 0 ||
            dict_set_interned(segment, ms->str_cue_out_explicitly_duration,
                              (flags & SEG_FLAG_CUE_OUT_EXPLICIT_DURATION) ? Py_True : Py_False) < 0 ||
            dict_set_interned(segment, ms->str_discontinuity,
                              (flags & SEG_FLAG_DISCONTINUITY) ? Py_True : Py_False) < 0 ||
            dict_set_interned(segment, ms->str_gap_tag,
                              (flags & SEG_FLAG_GAP) ? Py_True : Py_None) < 0)
        {
            goto fail_seg;
        }

        #define TRANSFER_SCTE_SLOW(src_field, dst_key) \
            do { \
                PyObject *v = ctx->src_field; \
                if (dict_set_interned(segment, ms->dst_key, v ? v : Py_None) < 0) goto fail_seg; \
                if (v != NULL && !cue_out_truth) { Py_CLEAR(ctx->src_field); } \
            } while (0)
        TRANSFER_SCTE_SLOW(current_cue_out_scte35, str_scte35);
        TRANSFER_SCTE_SLOW(current_cue_out_oatcls_scte35, str_oatcls_scte35);
        TRANSFER_SCTE_SLOW(current_cue_out_duration, str_scte35_duration);
        TRANSFER_SCTE_SLOW(current_cue_out_elapsedtime, str_scte35_elapsedtime);
        TRANSFER_SCTE_SLOW(asset_metadata, str_asset_metadata);
        #undef TRANSFER_SCTE_SLOW

        if (ctx->current_key != NULL) {
            if (dict_set_interned(segment, ms->str_key, ctx->current_key) < 0) goto fail_seg;
        } else if (!ctx->keys_has_none) {
            if (ensure_none_in_keys(ctx) < 0) goto fail_seg;
        }

        if (ctx->current_segment_map != NULL) {
            int has_map = PyDict_Check(ctx->current_segment_map)
                ? (PyDict_Size(ctx->current_segment_map) > 0)
                : PyObject_IsTrue(ctx->current_segment_map);
            if (has_map < 0) goto fail_seg;
            if (has_map &&
                dict_set_interned(segment, ms->str_init_section, ctx->current_segment_map) < 0)
            {
                goto fail_seg;
            }
        }

        if (dict_set_interned(segment, ms->str_dateranges,
                              ctx->dateranges ? ctx->dateranges : Py_None) < 0 ||
            dict_set_interned(segment, ms->str_blackout,
                              ctx->blackout ? ctx->blackout : Py_None) < 0)
        {
            goto fail_seg;
        }
        Py_CLEAR(ctx->dateranges);
        Py_CLEAR(ctx->blackout);
    }

    ctx->seg_flags = 0;
    ctx->expect_segment = 0;

    if (append_to_list(ctx->segments_list, segment, "data['segments'] must be a list") < 0) {
        Py_DECREF(segment);
        return -1;
    }
    Py_DECREF(segment);
    return 0;

fail_seg:
    Py_DECREF(segment);
    return -1;
}

static int
parse_variant_playlist(ParseContext *ctx, const char *line, size_t line_len)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *stream_info;
    if (ctx->stream_info != NULL) {
        stream_info = ctx->stream_info;
        ctx->stream_info = NULL;
    } else {
        stream_info = PyDict_New();
        if (stream_info == NULL) {
            return -1;
        }
    }

    PyObject *playlist = PyDict_New();
    if (playlist == NULL) {
        Py_DECREF(stream_info);
        return -1;
    }

    PyObject *uri = PyUnicode_FromStringAndSize(line, (Py_ssize_t)line_len);
    if (uri == NULL) {
        Py_DECREF(playlist);
        Py_DECREF(stream_info);
        return -1;
    }
    if (dict_set_interned(playlist, ms->str_uri, uri) < 0 ||
        dict_set_interned(playlist, ms->str_stream_info, stream_info) < 0)
    {
        Py_DECREF(uri);
        Py_DECREF(playlist);
        Py_DECREF(stream_info);
        return -1;
    }
    Py_DECREF(uri);
    Py_DECREF(stream_info);

    int rc = append_to_data_list(ctx, ms->str_playlists, playlist, "data['playlists'] must be a list");
    Py_DECREF(playlist);
    if (rc < 0) {
        return -1;
    }

    ctx->expect_playlist = 0;
    return 0;
}

/* ============================================================================
 * Tag Handlers (length-aware, zero-copy)
 * ============================================================================ */

static int
handle_program_date_time(ParseContext *ctx, const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *py_val = PyUnicode_FromStringAndSize(val, (Py_ssize_t)val_len);
    if (py_val == NULL) {
        return -1;
    }
    PyObject *dt = PyObject_CallFunctionObjArgs(ms->fromisoformat_meth, py_val, NULL);
    Py_DECREF(py_val);
    if (dt == NULL) {
        return -1;
    }

    PyObject *existing = dict_get_interned(ctx->data, ms->str_program_date_time);
    if (existing == NULL && PyErr_Occurred()) {
        Py_DECREF(dt);
        return -1;
    }
    if (existing == NULL || existing == Py_None) {
        if (dict_set_interned(ctx->data, ms->str_program_date_time, dt) < 0) {
            Py_DECREF(dt);
            return -1;
        }
    }

    /* Discard any deferred addition from the previous segment */
    ctx->has_pending_pdt_add = 0;
    pyobj_replace(&ctx->current_program_date_time, Py_NewRef(dt));
    pyobj_replace(&ctx->program_date_time, dt);
    return 0;
}

static int
handle_key(ParseContext *ctx, const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;

    /* Fast path: repeated identical #EXT-X-KEY line */
    if (ctx->last_key_val_ptr != NULL &&
        val_len == ctx->last_key_val_len &&
        memcmp(val, ctx->last_key_val_ptr, val_len) == 0)
    {
        PyObject *key_copy = PyDict_Copy(ctx->last_key_dict);
        if (key_copy == NULL) {
            return -1;
        }
        pyobj_replace(&ctx->current_key, key_copy);
        return 0;
    }

    PyObject *key = parse_attributes_with_schema(
        ms, val, val + val_len, key_parsers, NUM_KEY_PARSERS, 1);
    if (key == NULL) {
        return -1;
    }

    pyobj_replace(&ctx->current_key, Py_NewRef(key));

    PyObject *keys = ctx->keys_list;
    if (keys == NULL || !PyList_Check(keys)) {
        PyErr_SetString(PyExc_TypeError, "data['keys'] must be a list");
        Py_DECREF(key);
        return -1;
    }
    int found = PySequence_Contains(keys, key);
    if (found < 0) {
        Py_DECREF(key);
        return -1;
    }
    if (found == 0) {
        if (PyList_Append(keys, key) < 0) {
            Py_DECREF(key);
            return -1;
        }
    }

    ctx->last_key_val_ptr = val;
    ctx->last_key_val_len = val_len;
    pyobj_replace(&ctx->last_key_dict, key);
    return 0;
}

static int
handle_byterange(ParseContext *ctx, const char *val, size_t val_len)
{
    PyObject *segment = materialize_segment(ctx);
    if (segment == NULL) {
        return -1;
    }
    PyObject *py_value = PyUnicode_FromStringAndSize(val, (Py_ssize_t)val_len);
    if (py_value == NULL) {
        return -1;
    }
    int rc = dict_set_interned(segment, ctx->mod_state->str_byterange, py_value);
    Py_DECREF(py_value);
    if (rc < 0) {
        return -1;
    }
    ctx->expect_segment = 1;
    return 0;
}

static int
handle_bitrate(ParseContext *ctx, const char *val, size_t val_len)
{
    PyObject *segment = materialize_segment(ctx);
    if (segment == NULL) {
        return -1;
    }
    PyObject *py_value = parse_int_slice(val, val_len);
    if (py_value == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    int rc = dict_set_interned(segment, ctx->mod_state->str_bitrate, py_value);
    Py_DECREF(py_value);
    return rc < 0 ? -1 : 0;
}

static inline int
handle_data_int(ParseContext *ctx, const char *val, size_t val_len, PyObject *field_key)
{
    PyObject *py_value = parse_int_slice(val, val_len);
    if (py_value == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    int rc = dict_set_interned(ctx->data, field_key, py_value);
    Py_DECREF(py_value);
    return rc < 0 ? -1 : 0;
}

static int
handle_stream_inf(ParseContext *ctx, const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;
    ctx->expect_playlist = 1;
    if (dict_set_interned(ctx->data, ms->str_is_variant, Py_True) < 0 ||
        dict_set_interned(ctx->data, ms->str_media_sequence, Py_None) < 0)
    {
        return -1;
    }

    PyObject *stream_info = parse_attributes_with_schema(
        ms, val, val + val_len, stream_inf_parsers, NUM_STREAM_INF_PARSERS, 0);
    if (stream_info == NULL) {
        return -1;
    }
    pyobj_replace(&ctx->stream_info, stream_info);
    return 0;
}

static int
handle_stream_inf_with_uri(ParseContext *ctx, const char *val, size_t val_len,
                           const AttrParser *parsers, size_t num_parsers,
                           PyObject *info_key, PyObject *list_key)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *info = parse_attributes_with_schema(
        ms, val, val + val_len, parsers, num_parsers, 0);
    if (info == NULL) {
        return -1;
    }

    PyObject *uri = dict_get_interned(info, ms->str_uri);
    if (uri == NULL) {
        Py_DECREF(info);
        return 0;
    }
    Py_INCREF(uri);
    if (PyDict_DelItem(info, ms->str_uri) < 0) {
        Py_DECREF(uri);
        Py_DECREF(info);
        return -1;
    }

    PyObject *playlist = PyDict_New();
    if (playlist == NULL) {
        Py_DECREF(uri);
        Py_DECREF(info);
        return -1;
    }

    if (dict_set_interned(playlist, ms->str_uri, uri) < 0 ||
        dict_set_interned(playlist, info_key, info) < 0)
    {
        Py_DECREF(playlist);
        Py_DECREF(uri);
        Py_DECREF(info);
        return -1;
    }
    Py_DECREF(uri);
    Py_DECREF(info);

    int rc = append_to_data_list(ctx, list_key, playlist, "playlist target must be a list");
    Py_DECREF(playlist);
    return rc;
}

static int
handle_typed_attr_list(ParseContext *ctx, const char *val, size_t val_len,
                       const AttrParser *parsers, size_t num_parsers,
                       PyObject *list_key)
{
    PyObject *result = parse_attributes_with_schema(
        ctx->mod_state, val, val + val_len, parsers, num_parsers, 0);
    if (result == NULL) {
        return -1;
    }
    int rc = append_to_data_list(ctx, list_key, result, "playlist target must be a list");
    Py_DECREF(result);
    return rc;
}

static int
handle_typed_attr_dict(ParseContext *ctx, const char *val, size_t val_len,
                       const AttrParser *parsers, size_t num_parsers,
                       PyObject *field_key)
{
    PyObject *result = parse_attributes_with_schema(
        ctx->mod_state, val, val + val_len, parsers, num_parsers, 0);
    if (result == NULL) {
        return -1;
    }
    int rc = dict_set_interned(ctx->data, field_key, result);
    Py_DECREF(result);
    return rc < 0 ? -1 : 0;
}

static int
handle_part(ParseContext *ctx, const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *part = parse_attributes_with_schema(
        ms, val, val + val_len, part_parsers, NUM_PART_PARSERS, 0);
    if (part == NULL) {
        return -1;
    }

    if (ctx->has_pending_pdt_add && flush_pending_pdt(ctx) < 0) {
        Py_DECREF(part);
        return -1;
    }

    if (ctx->current_program_date_time != NULL) {
        if (dict_set_interned(part, ms->str_program_date_time,
                              ctx->current_program_date_time) < 0)
        {
            Py_DECREF(part);
            return -1;
        }
        PyObject *duration = dict_get_interned(part, ms->str_duration);
        if (duration != NULL) {
            double secs = PyFloat_AsDouble(duration);
            if (secs == -1.0 && PyErr_Occurred()) {
                Py_DECREF(part);
                return -1;
            }
            if (isfinite(secs) && fabs(secs) < 1e12) {
                ctx->has_pending_pdt_add = 1;
                ctx->pending_pdt_secs = secs;
            } else {
                PyObject *new_pdt = datetime_add_seconds(
                    ctx, ctx->current_program_date_time, secs);
                if (new_pdt == NULL) {
                    Py_DECREF(part);
                    return -1;
                }
                pyobj_replace(&ctx->current_program_date_time, new_pdt);
            }
        }
    }

    if (dict_set_interned(part, ms->str_dateranges,
                          ctx->dateranges ? ctx->dateranges : Py_None) < 0)
    {
        Py_DECREF(part);
        return -1;
    }
    Py_CLEAR(ctx->dateranges);

    int has_gap = (ctx->seg_flags & SEG_FLAG_GAP) != 0;
    if (dict_set_interned(part, ms->str_gap_tag, has_gap ? Py_True : Py_None) < 0) {
        Py_DECREF(part);
        return -1;
    }
    ctx->seg_flags &= ~SEG_FLAG_GAP;

    PyObject *segment = materialize_segment(ctx);
    if (segment == NULL) {
        Py_DECREF(part);
        return -1;
    }

    PyObject *parts = dict_get_interned(segment, ms->str_parts);
    if (parts == NULL) {
        if (PyErr_Occurred()) {
            Py_DECREF(part);
            return -1;
        }
        parts = PyList_New(0);
        if (parts == NULL) {
            Py_DECREF(part);
            return -1;
        }
        if (dict_set_interned(segment, ms->str_parts, parts) < 0) {
            Py_DECREF(parts);
            Py_DECREF(part);
            return -1;
        }
        int rc = PyList_Append(parts, part);
        Py_DECREF(parts);
        Py_DECREF(part);
        return rc;
    }

    int rc = append_to_list(parts, part, "segment['parts'] must be a list");
    Py_DECREF(part);
    return rc;
}

static int
handle_cue_out(ParseContext *ctx, const char *line, size_t line_len,
               int has_colon, const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;
    ctx->seg_flags |= (SEG_FLAG_CUE_OUT_START | SEG_FLAG_CUE_OUT);

    if (ascii_contains_ci_upper(line, line_len, "DURATION", 8)) {
        ctx->seg_flags |= SEG_FLAG_CUE_OUT_EXPLICIT_DURATION;
    }

    if (!has_colon || val_len == 0) {
        return 0;
    }

    PyObject *cue_info = parse_attributes_with_schema(
        ms, val, val + val_len, cueout_parsers, NUM_CUEOUT_PARSERS, 0);
    if (cue_info == NULL) {
        return -1;
    }

    PyObject *cue_out_scte35 = dict_get_interned(cue_info, ms->str_cue);
    PyObject *cue_out_duration = dict_get_interned(cue_info, ms->str_duration);
    if (cue_out_duration == NULL) {
        cue_out_duration = dict_get_interned(cue_info, ms->str_empty);
    }

    if (cue_out_scte35 != NULL) {
        pyobj_replace(&ctx->current_cue_out_scte35, Py_NewRef(cue_out_scte35));
    }
    pyobj_replace(&ctx->current_cue_out_duration, Py_XNewRef(cue_out_duration));

    Py_DECREF(cue_info);
    return 0;
}

static int
handle_cue_out_cont(ParseContext *ctx, int has_colon, const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;
    ctx->seg_flags |= SEG_FLAG_CUE_OUT;

    if (!has_colon || val_len == 0) {
        return 0;
    }

    PyObject *cue_info = parse_attributes_with_schema(
        ms, val, val + val_len, cueout_cont_parsers, NUM_CUEOUT_CONT_PARSERS, 0);
    if (cue_info == NULL) {
        return -1;
    }

    PyObject *progress = dict_get_interned(cue_info, ms->str_empty);
    if (progress != NULL) {
        if (!PyUnicode_Check(progress)) {
            PyErr_SetString(PyExc_TypeError, "expected str for cue-out progress");
            Py_DECREF(cue_info);
            return -1;
        }
        Py_ssize_t n = PyUnicode_GetLength(progress);
        Py_ssize_t slash = PyUnicode_FindChar(progress, '/', 0, n, 1);
        if (slash >= 0) {
            PyObject *elapsed = PyUnicode_Substring(progress, 0, slash);
            PyObject *duration = PyUnicode_Substring(progress, slash + 1, n);
            if (elapsed == NULL || duration == NULL) {
                Py_XDECREF(elapsed);
                Py_XDECREF(duration);
                Py_DECREF(cue_info);
                return -1;
            }
            pyobj_replace(&ctx->current_cue_out_elapsedtime, elapsed);
            pyobj_replace(&ctx->current_cue_out_duration, duration);
        } else {
            pyobj_replace(&ctx->current_cue_out_duration, Py_NewRef(progress));
        }
    }

    PyObject *duration = dict_get_interned(cue_info, ms->str_duration);
    if (duration != NULL) {
        pyobj_replace(&ctx->current_cue_out_duration, Py_NewRef(duration));
    }

    PyObject *scte35 = dict_get_interned(cue_info, ms->str_scte35);
    if (scte35 != NULL) {
        pyobj_replace(&ctx->current_cue_out_scte35, Py_NewRef(scte35));
    }

    PyObject *elapsedtime = dict_get_interned(cue_info, ms->str_elapsedtime);
    if (elapsedtime != NULL) {
        pyobj_replace(&ctx->current_cue_out_elapsedtime, Py_NewRef(elapsedtime));
    }

    Py_DECREF(cue_info);
    return 0;
}

static int
handle_oatcls_scte35(ParseContext *ctx, const char *val, size_t val_len)
{
    PyObject *py_value = PyUnicode_FromStringAndSize(val, (Py_ssize_t)val_len);
    if (py_value == NULL) {
        return -1;
    }
    pyobj_replace(&ctx->current_cue_out_oatcls_scte35, Py_NewRef(py_value));
    if (ctx->current_cue_out_scte35 == NULL) {
        pyobj_replace(&ctx->current_cue_out_scte35, py_value);
    } else {
        Py_DECREF(py_value);
    }
    return 0;
}

static int
handle_asset(ParseContext *ctx, const char *val, size_t val_len)
{
    PyObject *asset = parse_attributes_with_schema(
        ctx->mod_state, val, val + val_len, NULL, 0, 0);
    if (asset == NULL) {
        return -1;
    }
    pyobj_replace(&ctx->asset_metadata, asset);
    return 0;
}

static int
handle_map(ParseContext *ctx, const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *map_info = parse_attributes_with_schema(
        ms, val, val + val_len, x_map_parsers, NUM_X_MAP_PARSERS, 0);
    if (map_info == NULL) {
        return -1;
    }
    pyobj_replace(&ctx->current_segment_map, Py_NewRef(map_info));
    int rc = append_to_data_list(ctx, ms->str_segment_map, map_info, "data['segment_map'] must be a list");
    Py_DECREF(map_info);
    return rc;
}

static int
handle_session_key(ParseContext *ctx, const char *val, size_t val_len)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *key = parse_attributes_with_schema(
        ms, val, val + val_len, key_parsers, NUM_KEY_PARSERS, 1);
    if (key == NULL) {
        return -1;
    }
    int rc = append_to_data_list(ctx, ms->str_session_keys, key, "data['session_keys'] must be a list");
    Py_DECREF(key);
    return rc;
}

static int
handle_daterange(ParseContext *ctx, const char *val, size_t val_len)
{
    PyObject *daterange = parse_attributes_with_schema(
        ctx->mod_state, val, val + val_len, daterange_parsers, NUM_DATERANGE_PARSERS, 0);
    if (daterange == NULL) {
        return -1;
    }
    if (ctx->dateranges == NULL) {
        ctx->dateranges = PyList_New(0);
        if (ctx->dateranges == NULL) {
            Py_DECREF(daterange);
            return -1;
        }
    }
    int rc = append_to_list(ctx->dateranges, daterange, "state['dateranges'] must be a list");
    Py_DECREF(daterange);
    return rc;
}

static int
handle_blackout(ParseContext *ctx, int has_colon, const char *val, size_t val_len)
{
    if (has_colon && val_len > 0) {
        PyObject *blackout_data = PyUnicode_FromStringAndSize(val, (Py_ssize_t)val_len);
        if (blackout_data == NULL) {
            return -1;
        }
        pyobj_replace(&ctx->blackout, blackout_data);
    } else {
        pyobj_replace(&ctx->blackout, Py_NewRef(Py_True));
    }
    return 0;
}

static int
handle_lower_stripped_data(ParseContext *ctx, const char *val, size_t val_len,
                           PyObject *field_key)
{
    PyObject *py_value = create_lower_stripped(val, val_len);
    if (py_value == NULL) {
        return -1;
    }
    int rc = dict_set_interned(ctx->data, field_key, py_value);
    Py_DECREF(py_value);
    return rc < 0 ? -1 : 0;
}

/*
 * O(1) tag dispatcher.
 *
 * Splits tag/value on the first ':', fast-paths #EXTINF, then dispatches all
 * #EXT-X-* tags via switch on line[7] + exact suffix length (every bucket has
 * unique lengths, so at most one memcmp is ever executed per line).
 *
 * Returns:
 *   1 if handled (including #EXTM3U)
 *   0 if unknown tag
 *  -1 on error (exception set)
 */
static int
dispatch_tag(ParseContext *ctx, const char *line, size_t line_len)
{
    if (line_len < 7) {
        return 0;
    }

    const char *colon = memchr(line, ':', line_len);
    size_t tag_len = colon ? (size_t)(colon - line) : line_len;
    int has_colon = (colon != NULL);
    const char *val = has_colon ? (colon + 1) : NULL;
    size_t val_len = has_colon ? (line_len - tag_len - 1) : 0;

    /* Fast path #1: #EXTINF (hottest tag in media playlists) */
    if (tag_len == 7 && has_colon && memcmp(line, "#EXTINF", 7) == 0) {
        return handle_extinf(ctx, line, line_len, val, val_len) < 0 ? -1 : 1;
    }

    /* Fast path #2: #EXT-X-* tags (39 of 42 protocol tags) */
    if (tag_len > 7 && memcmp(line, "#EXT-X-", 7) == 0) {
        m3u8_state *ms = ctx->mod_state;
        const char *suf = line + 7;
        size_t slen = tag_len - 7;

        #define MATCH_VAL(lit) \
            (has_colon && slen == (sizeof(lit) - 1) && memcmp(suf, lit, sizeof(lit) - 1) == 0)
        #define MATCH_BARE(lit) \
            (slen == (sizeof(lit) - 1) && memcmp(suf, lit, sizeof(lit) - 1) == 0)

        switch (suf[0]) {
        case 'P':
            if (MATCH_VAL("PROGRAM-DATE-TIME")) {
                return handle_program_date_time(ctx, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("PART")) {
                return handle_part(ctx, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("PLAYLIST-TYPE")) {
                return handle_lower_stripped_data(ctx, val, val_len, ms->str_playlist_type) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("PART-INF")) {
                return handle_typed_attr_dict(ctx, val, val_len, part_inf_parsers,
                                              NUM_PART_INF_PARSERS, ms->str_part_inf) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("PRELOAD-HINT")) {
                return handle_typed_attr_dict(ctx, val, val_len, preload_hint_parsers,
                                              NUM_PRELOAD_HINT_PARSERS, ms->str_preload_hint) < 0 ? -1 : 1;
            }
            return 0;

        case 'D':
            if (MATCH_BARE("DISCONTINUITY")) {
                ctx->seg_flags |= SEG_FLAG_DISCONTINUITY;
                return 1;
            }
            if (MATCH_VAL("DATERANGE")) {
                return handle_daterange(ctx, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("DISCONTINUITY-SEQUENCE")) {
                return handle_data_int(ctx, val, val_len, ms->str_discontinuity_sequence) < 0 ? -1 : 1;
            }
            return 0;

        case 'K':
            if (MATCH_VAL("KEY")) {
                return handle_key(ctx, val, val_len) < 0 ? -1 : 1;
            }
            return 0;

        case 'B':
            if (MATCH_VAL("BYTERANGE")) {
                return handle_byterange(ctx, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("BITRATE")) {
                return handle_bitrate(ctx, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_BARE("BLACKOUT")) {
                return handle_blackout(ctx, has_colon, val, val_len) < 0 ? -1 : 1;
            }
            return 0;

        case 'C':
            if (MATCH_BARE("CUE-OUT-CONT")) {
                return handle_cue_out_cont(ctx, has_colon, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_BARE("CUE-OUT")) {
                return handle_cue_out(ctx, line, line_len, has_colon, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_BARE("CUE-IN")) {
                ctx->seg_flags |= SEG_FLAG_CUE_IN;
                return 1;
            }
            if (MATCH_BARE("CUE-SPAN")) {
                ctx->seg_flags |= SEG_FLAG_CUE_OUT;
                return 1;
            }
            if (MATCH_VAL("CONTENT-STEERING")) {
                return handle_typed_attr_dict(ctx, val, val_len, content_steering_parsers,
                                              NUM_CONTENT_STEERING_PARSERS,
                                              ms->str_content_steering) < 0 ? -1 : 1;
            }
            return 0;

        case 'M':
            if (MATCH_VAL("MAP")) {
                return handle_map(ctx, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("MEDIA")) {
                return handle_typed_attr_list(ctx, val, val_len, media_parsers,
                                              NUM_MEDIA_PARSERS, ms->str_media) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("MEDIA-SEQUENCE")) {
                return handle_data_int(ctx, val, val_len, ms->str_media_sequence) < 0 ? -1 : 1;
            }
            return 0;

        case 'S':
            if (MATCH_VAL("STREAM-INF")) {
                return handle_stream_inf(ctx, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("START")) {
                return handle_typed_attr_dict(ctx, val, val_len, start_parsers,
                                              NUM_START_PARSERS, ms->str_start) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("SERVER-CONTROL")) {
                return handle_typed_attr_dict(ctx, val, val_len, server_control_parsers,
                                              NUM_SERVER_CONTROL_PARSERS,
                                              ms->str_server_control) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("SKIP")) {
                return handle_typed_attr_dict(ctx, val, val_len, skip_parsers,
                                              NUM_SKIP_PARSERS, ms->str_skip) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("SESSION-DATA")) {
                return handle_typed_attr_list(ctx, val, val_len, session_data_parsers,
                                              NUM_SESSION_DATA_PARSERS,
                                              ms->str_session_data) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("SESSION-KEY")) {
                return handle_session_key(ctx, val, val_len) < 0 ? -1 : 1;
            }
            return 0;

        case 'T':
            if (MATCH_VAL("TARGETDURATION")) {
                return handle_data_int(ctx, val, val_len, ms->str_targetduration) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("TILES")) {
                return handle_typed_attr_list(ctx, val, val_len, tiles_parsers,
                                              NUM_TILES_PARSERS, ms->str_tiles) < 0 ? -1 : 1;
            }
            return 0;

        case 'V':
            if (MATCH_VAL("VERSION")) {
                return handle_data_int(ctx, val, val_len, ms->str_version) < 0 ? -1 : 1;
            }
            return 0;

        case 'E':
            if (MATCH_BARE("ENDLIST")) {
                return dict_set_interned(ctx->data, ms->str_is_endlist, Py_True) < 0 ? -1 : 1;
            }
            return 0;

        case 'G':
            if (MATCH_BARE("GAP")) {
                ctx->seg_flags |= SEG_FLAG_GAP;
                return 1;
            }
            return 0;

        case 'A':
            if (MATCH_VAL("ASSET")) {
                return handle_asset(ctx, val, val_len) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("ALLOW-CACHE")) {
                return handle_lower_stripped_data(ctx, val, val_len, ms->str_allow_cache) < 0 ? -1 : 1;
            }
            return 0;

        case 'I':
            if (MATCH_BARE("I-FRAMES-ONLY")) {
                return dict_set_interned(ctx->data, ms->str_is_i_frames_only, Py_True) < 0 ? -1 : 1;
            }
            if (MATCH_BARE("INDEPENDENT-SEGMENTS")) {
                return dict_set_interned(ctx->data, ms->str_is_independent_segments, Py_True) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("I-FRAME-STREAM-INF")) {
                return handle_stream_inf_with_uri(
                    ctx, val, val_len, iframe_stream_inf_parsers, NUM_IFRAME_STREAM_INF_PARSERS,
                    ms->str_iframe_stream_info, ms->str_iframe_playlists) < 0 ? -1 : 1;
            }
            if (MATCH_VAL("IMAGE-STREAM-INF")) {
                return handle_stream_inf_with_uri(
                    ctx, val, val_len, image_stream_inf_parsers, NUM_IMAGE_STREAM_INF_PARSERS,
                    ms->str_image_stream_info, ms->str_image_playlists) < 0 ? -1 : 1;
            }
            if (MATCH_BARE("IMAGES-ONLY")) {
                return dict_set_interned(ctx->data, ms->str_is_images_only, Py_True) < 0 ? -1 : 1;
            }
            return 0;

        case 'R':
            if (MATCH_VAL("RENDITION-REPORT")) {
                return handle_typed_attr_list(ctx, val, val_len, rendition_report_parsers,
                                              NUM_RENDITION_REPORT_PARSERS,
                                              ms->str_rendition_reports) < 0 ? -1 : 1;
            }
            return 0;

        default:
            return 0;
        }

        #undef MATCH_VAL
        #undef MATCH_BARE
    }

    /* Remaining non-#EXT-X- tags */
    if (tag_len == 7 && memcmp(line, "#EXTM3U", 7) == 0) {
        return 1;
    }
    if (tag_len == 18 && has_colon && memcmp(line, "#EXT-OATCLS-SCTE35", 18) == 0) {
        return handle_oatcls_scte35(ctx, val, val_len) < 0 ? -1 : 1;
    }

    return 0;
}

/*
 * Main parse function: parse(content, strict=False, custom_tags_parser=None)
 */
static PyObject *
m3u8_parse(PyObject *module, PyObject *args, PyObject *kwargs)
{
    const char *content;
    Py_ssize_t content_len;
    int strict = 0;
    PyObject *custom_tags_parser = Py_None;

    static char *kwlist[] = {"content", "strict", "custom_tags_parser", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#|pO", kwlist,
                                     &content, &content_len, &strict, &custom_tags_parser))
    {
        return NULL;
    }

    if (memchr(content, '\0', (size_t)content_len) != NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "embedded null bytes are not supported by the C parser");
        return NULL;
    }

    /* Match parser.py: lines = content.strip().splitlines() */
    const char *trimmed = content;
    const char *trimmed_end = content + content_len;
    while (trimmed < trimmed_end && ascii_isspace((unsigned char)*trimmed)) {
        trimmed++;
    }
    while (trimmed_end > trimmed && ascii_isspace((unsigned char)*(trimmed_end - 1))) {
        trimmed_end--;
    }
    size_t trimmed_len = (size_t)(trimmed_end - trimmed);

    m3u8_state *mod_state = get_m3u8_state(module);

    if (strict) {
        PyObject *version_matching = PyImport_ImportModule("openm3u8.version_matching");
        if (version_matching == NULL) {
            return NULL;
        }
        PyObject *validate = PyObject_GetAttrString(version_matching, "validate");
        if (validate == NULL) {
            Py_DECREF(version_matching);
            return NULL;
        }
        PyObject *lines_list = build_stripped_splitlines(trimmed, trimmed_len);
        if (lines_list == NULL) {
            Py_DECREF(validate);
            Py_DECREF(version_matching);
            return NULL;
        }

        PyObject *errors = PyObject_CallFunctionObjArgs(validate, lines_list, NULL);
        Py_DECREF(lines_list);
        Py_DECREF(validate);
        Py_DECREF(version_matching);

        if (errors == NULL) {
            return NULL;
        }
        if (PyList_Size(errors) > 0) {
            PyErr_SetObject(PyExc_Exception, errors);
            Py_DECREF(errors);
            return NULL;
        }
        Py_DECREF(errors);
    }

    PyObject *data = init_parse_data(mod_state);
    if (data == NULL) {
        return NULL;
    }

    int has_custom_parser = (custom_tags_parser != Py_None && PyCallable_Check(custom_tags_parser));

    ParseContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod_state = mod_state;
    ctx.data = data;
    ctx.segments_list = dict_get_interned(data, mod_state->str_segments);
    ctx.keys_list = dict_get_interned(data, mod_state->str_keys);
    ctx.strict = strict;

    /* Single SIMD upfront check for CR so common LF-only playlists need only one memchr/line */
    int has_cr = (trimmed_len > 0 && memchr(trimmed, '\r', trimmed_len) != NULL);

    const char *p = trimmed;
    const char *end = trimmed + trimmed_len;

    while (p < end) {
        ctx.lineno++;

        const char *line_start = p;
        const char *eol;
        size_t remaining = (size_t)(end - p);

        if (!has_cr) {
            eol = memchr(p, '\n', remaining);
            if (eol == NULL) {
                eol = end;
                p = end;
            } else {
                p = eol + 1;
            }
        } else {
            const char *nl = memchr(p, '\n', remaining);
            size_t search_len = nl ? (size_t)(nl - p) : remaining;
            const char *cr = memchr(p, '\r', search_len);
            if (cr != NULL) {
                eol = cr;
                if (cr + 1 < end && *(cr + 1) == '\n') {
                    p = cr + 2;
                } else {
                    p = cr + 1;
                }
            } else if (nl != NULL) {
                eol = nl;
                p = nl + 1;
            } else {
                eol = end;
                p = end;
            }
        }

        size_t line_len = (size_t)(eol - line_start);

        /* Strip leading/trailing ASCII whitespace in-place via pointer/length */
        while (line_len > 0 && ascii_isspace((unsigned char)*line_start)) {
            line_start++;
            line_len--;
        }
        while (line_len > 0 && ascii_isspace((unsigned char)line_start[line_len - 1])) {
            line_len--;
        }

        if (line_len == 0) {
            continue;
        }

        if (line_start[0] == '#') {
            if (has_custom_parser) {
                if (sync_ctx_to_state_dict(&ctx) < 0) {
                    goto fail;
                }
                PyObject *py_line = PyUnicode_FromStringAndSize(line_start, (Py_ssize_t)line_len);
                PyObject *py_lineno = PyLong_FromLong(ctx.lineno);
                if (py_line == NULL || py_lineno == NULL) {
                    Py_XDECREF(py_line);
                    Py_XDECREF(py_lineno);
                    goto fail;
                }
                PyObject *result = PyObject_CallFunctionObjArgs(
                    custom_tags_parser, py_line, py_lineno, data, ctx.state, NULL);
                Py_DECREF(py_line);
                Py_DECREF(py_lineno);
                if (result == NULL) {
                    goto fail;
                }
                int truth = PyObject_IsTrue(result);
                Py_DECREF(result);
                if (truth < 0) {
                    goto fail;
                }
                if (sync_state_dict_to_ctx(&ctx) < 0) {
                    goto fail;
                }
                if (truth) {
                    continue;
                }
            }

            int dispatch_result = dispatch_tag(&ctx, line_start, line_len);
            if (dispatch_result < 0) {
                goto fail;
            }
            if (dispatch_result == 0 && strict) {
                raise_parse_error(mod_state, ctx.lineno, line_start, line_len);
                goto fail;
            }
        } else {
            if (ctx.expect_segment) {
                if (parse_ts_chunk(&ctx, line_start, line_len) < 0) {
                    goto fail;
                }
            } else if (ctx.expect_playlist) {
                if (parse_variant_playlist(&ctx, line_start, line_len) < 0) {
                    goto fail;
                }
            } else if (strict) {
                raise_parse_error(mod_state, ctx.lineno, line_start, line_len);
                goto fail;
            }
        }
    }

    /* Handle remaining partial segment at EOF */
    if (ctx.has_extinf && materialize_segment(&ctx) == NULL) {
        goto fail;
    }
    if (ctx.segment != NULL) {
        if (append_to_list(ctx.segments_list, ctx.segment, "data['segments'] must be a list") < 0) {
            goto fail;
        }
    }

    parse_context_clear(&ctx);
    return data;

fail:
    parse_context_clear(&ctx);
    Py_DECREF(data);
    return NULL;
}

static PyMethodDef m3u8_parser_methods[] = {
    {"parse", (PyCFunction)(void (*)(void))m3u8_parse, METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR(
     "parse(content, strict=False, custom_tags_parser=None)\n"
     "--\n\n"
     "Parse M3U8 playlist content and return a dictionary with all data found.\n\n"
     "This is an optimized C implementation that produces output identical to\n"
     "the pure Python parser in openm3u8.parser.parse().\n\n"
     "Parameters\n"
     "----------\n"
     "content : str\n"
     "    The M3U8 playlist content as a string.\n"
     "strict : bool, optional\n"
     "    If True, raise exceptions for syntax errors. Default is False.\n"
     "custom_tags_parser : callable, optional\n"
     "    A function that receives (line, lineno, data, state) for custom tag\n"
     "    handling. Return True to skip default parsing for that line.\n\n"
     "Returns\n"
     "-------\n"
     "dict\n"
     "    A dictionary containing the parsed playlist data with keys including:\n"
     "    'segments', 'playlists', 'media', 'keys', 'is_variant', etc.\n\n"
     "Raises\n"
     "------\n"
     "ParseError\n"
     "    If strict=True and a syntax error is encountered.\n"
     "Exception\n"
     "    If strict=True and version validation fails.\n\n"
     "Examples\n"
     "--------\n"
     ">>> from openm3u8._m3u8_parser import parse\n"
     ">>> result = parse('#EXTM3U\\n#EXTINF:10,\\nfoo.ts')\n"
     ">>> len(result['segments'])\n"
     "1\n"
     )},
    {NULL, NULL, 0, NULL}
};

static int
m3u8_parser_traverse(PyObject *module, visitproc visit, void *arg)
{
    m3u8_state *state = get_m3u8_state(module);
    Py_VISIT(state->ParseError);
    Py_VISIT(state->datetime_cls);
    Py_VISIT(state->timedelta_cls);
    Py_VISIT(state->fromisoformat_meth);
    Py_VISIT(state->int_zero);
    Py_VISIT(state->float_zero);
    Py_VISIT(state->segment_template);
    #define VISIT_INTERNED(name, str) Py_VISIT(state->name);
    INTERNED_STRINGS(VISIT_INTERNED)
    #undef VISIT_INTERNED
    return 0;
}

static int
m3u8_parser_clear(PyObject *module)
{
    m3u8_state *state = get_m3u8_state(module);
    Py_CLEAR(state->ParseError);
    Py_CLEAR(state->datetime_cls);
    Py_CLEAR(state->timedelta_cls);
    Py_CLEAR(state->fromisoformat_meth);
    Py_CLEAR(state->int_zero);
    Py_CLEAR(state->float_zero);
    Py_CLEAR(state->segment_template);
    #define CLEAR_INTERNED(name, str) Py_CLEAR(state->name);
    INTERNED_STRINGS(CLEAR_INTERNED)
    #undef CLEAR_INTERNED
    return 0;
}

static void
m3u8_parser_free(void *module)
{
    m3u8_parser_clear((PyObject *)module);
}

static struct PyModuleDef m3u8_parser_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_m3u8_parser",
    .m_doc = "C extension for fast M3U8 playlist parsing.",
    .m_size = sizeof(m3u8_state),
    .m_methods = m3u8_parser_methods,
    .m_traverse = m3u8_parser_traverse,
    .m_clear = m3u8_parser_clear,
    .m_free = m3u8_parser_free,
};

PyMODINIT_FUNC
PyInit__m3u8_parser(void)
{
    PyObject *m = PyModule_Create(&m3u8_parser_module);
    if (m == NULL) {
        return NULL;
    }

    m3u8_state *state = get_m3u8_state(m);
    memset(state, 0, sizeof(*state));

    PyObject *parser_module = PyImport_ImportModule("openm3u8.parser");
    if (parser_module != NULL) {
        state->ParseError = PyObject_GetAttrString(parser_module, "ParseError");
        Py_DECREF(parser_module);
    }

    if (state->ParseError == NULL) {
        PyErr_Clear();
        state->ParseError = PyErr_NewException(
            "openm3u8._m3u8_parser.ParseError", PyExc_Exception, NULL);
        if (state->ParseError == NULL) {
            goto error;
        }
    }

    if (PyModule_AddObjectRef(m, "ParseError", state->ParseError) < 0) {
        goto error;
    }

    /* Intern strings first so init_cached_objects can populate segment_template */
    if (init_interned_strings(state) < 0) {
        goto error;
    }

    if (init_cached_objects(state) < 0) {
        goto error;
    }

    return m;

error:
    Py_DECREF(m);
    return NULL;
}
