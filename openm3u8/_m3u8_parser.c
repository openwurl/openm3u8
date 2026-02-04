/*
 * Copyright 2014 Globo.com Player authors. All rights reserved.
 * Modifications Copyright (c) 2026 Wurl.
 * Use of this source code is governed by a MIT License
 * license that can be found in the LICENSE file.
 *
 * C extension for m3u8 parser - provides optimized parsing of M3U8 playlists.
 *
 * This module implements the same parsing logic as m3u8/parser.py but in C
 * for improved performance. The output is designed to be identical to the
 * Python implementation.
 *
 * Design notes (following CPython extension best practices per PEP 7):
 *
 * Memory Management:
 * - Uses module state instead of static globals for subinterpreter safety
 *   (PEP 573, PEP 3121)
 * - Uses PyMem_* allocators consistently for better debugging/tracing
 * - Single cleanup path via goto for reliable resource management
 * - All borrowed references are clearly documented
 *
 * Performance Optimizations:
 * - Frequently-used dict keys are cached as interned strings
 * - Attribute parsers are const static arrays built at compile time
 * - String operations use restrict pointers where applicable
 *
 * Error Handling:
 * - All Python C API calls that can fail are checked
 * - Helper macros (DICT_SET_AND_DECREF) ensure consistent cleanup
 * - ParseError exception is shared with the Python parser module
 *
 * Thread Safety:
 * - No mutable static state; all state is per-module
 * - GIL is held throughout parsing (no release/acquire)
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/*
 * Whitespace/Case handling for protocol parsing.
 *
 * We intentionally treat playlist syntax as ASCII per RFC 8216. Using the C
 * locale-dependent ctype tables (isspace/tolower) can cause surprising
 * behavior changes depending on process locale and input bytes > 0x7F.
 *
 * Note: This does not attempt to mirror Python's full Unicode whitespace
 * semantics for str.strip(); the HLS grammar is ASCII and the input here is
 * parsed as UTF-8 bytes.
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
 * Case-insensitive match between a raw buffer and a null-terminated key.
 * Also treats '-' as '_' to match normalized attribute names.
 * This avoids creating Python objects during schema lookup (hot path optimization).
 */
static inline int
buffer_matches_key(const char *buf, size_t len, const char *key)
{
    for (size_t i = 0; i < len; i++) {
        if (key[i] == '\0') return 0;  /* Key shorter than buffer */
        unsigned char c = (unsigned char)buf[i];
        /* Normalize: lowercase and treat '-' as '_' */
        if (c == '-') c = '_';
        else c = ascii_tolower(c);
        if (c != (unsigned char)key[i]) return 0;
    }
    return key[len] == '\0';  /* Ensure exact length match */
}

/*
 * Compatibility shims for Py_NewRef/Py_XNewRef (added in Python 3.10).
 * These make reference ownership more explicit at call sites.
 */
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

/*
 * Forward declarations for inline helpers used before their definitions.
 */
static inline int dict_set_interned(PyObject *dict, PyObject *interned_key, PyObject *value);
static inline PyObject *dict_get_interned(PyObject *dict, PyObject *interned_key);

/*
 * Helper macro for setting dict items with proper error handling.
 * Decrefs value and returns/gotos on failure.
 */
#define DICT_SET_AND_DECREF(dict, key, value, cleanup_label) \
    do { \
        if (PyDict_SetItemString((dict), (key), (value)) < 0) { \
            Py_DECREF(value); \
            goto cleanup_label; \
        } \
        Py_DECREF(value); \
    } while (0)

/* Protocol tag definitions - must match protocol.py */
#define EXT_M3U "#EXTM3U"
#define EXT_X_TARGETDURATION "#EXT-X-TARGETDURATION"
#define EXT_X_MEDIA_SEQUENCE "#EXT-X-MEDIA-SEQUENCE"
#define EXT_X_DISCONTINUITY_SEQUENCE "#EXT-X-DISCONTINUITY-SEQUENCE"
#define EXT_X_PROGRAM_DATE_TIME "#EXT-X-PROGRAM-DATE-TIME"
#define EXT_X_MEDIA "#EXT-X-MEDIA"
#define EXT_X_PLAYLIST_TYPE "#EXT-X-PLAYLIST-TYPE"
#define EXT_X_KEY "#EXT-X-KEY"
#define EXT_X_STREAM_INF "#EXT-X-STREAM-INF"
#define EXT_X_VERSION "#EXT-X-VERSION"
#define EXT_X_ALLOW_CACHE "#EXT-X-ALLOW-CACHE"
#define EXT_X_ENDLIST "#EXT-X-ENDLIST"
#define EXTINF "#EXTINF"
#define EXT_I_FRAMES_ONLY "#EXT-X-I-FRAMES-ONLY"
#define EXT_X_ASSET "#EXT-X-ASSET"
#define EXT_X_BITRATE "#EXT-X-BITRATE"
#define EXT_X_BYTERANGE "#EXT-X-BYTERANGE"
#define EXT_X_I_FRAME_STREAM_INF "#EXT-X-I-FRAME-STREAM-INF"
#define EXT_X_DISCONTINUITY "#EXT-X-DISCONTINUITY"
#define EXT_X_CUE_OUT "#EXT-X-CUE-OUT"
#define EXT_X_CUE_OUT_CONT "#EXT-X-CUE-OUT-CONT"
#define EXT_X_CUE_IN "#EXT-X-CUE-IN"
#define EXT_X_CUE_SPAN "#EXT-X-CUE-SPAN"
#define EXT_OATCLS_SCTE35 "#EXT-OATCLS-SCTE35"
#define EXT_IS_INDEPENDENT_SEGMENTS "#EXT-X-INDEPENDENT-SEGMENTS"
#define EXT_X_MAP "#EXT-X-MAP"
#define EXT_X_START "#EXT-X-START"
#define EXT_X_SERVER_CONTROL "#EXT-X-SERVER-CONTROL"
#define EXT_X_PART_INF "#EXT-X-PART-INF"
#define EXT_X_PART "#EXT-X-PART"
#define EXT_X_RENDITION_REPORT "#EXT-X-RENDITION-REPORT"
#define EXT_X_SKIP "#EXT-X-SKIP"
#define EXT_X_SESSION_DATA "#EXT-X-SESSION-DATA"
#define EXT_X_SESSION_KEY "#EXT-X-SESSION-KEY"
#define EXT_X_PRELOAD_HINT "#EXT-X-PRELOAD-HINT"
#define EXT_X_DATERANGE "#EXT-X-DATERANGE"
#define EXT_X_GAP "#EXT-X-GAP"
#define EXT_X_CONTENT_STEERING "#EXT-X-CONTENT-STEERING"
#define EXT_X_IMAGE_STREAM_INF "#EXT-X-IMAGE-STREAM-INF"
#define EXT_X_IMAGES_ONLY "#EXT-X-IMAGES-ONLY"
#define EXT_X_TILES "#EXT-X-TILES"
#define EXT_X_BLACKOUT "#EXT-X-BLACKOUT"

/*
 * X-macro for interned strings.
 *
 * This eliminates 4x duplication: struct fields, init, traverse, clear.
 * Define once, expand everywhere with different operations.
 *
 * Format: X(field_name, string_value)
 */
#define INTERNED_STRINGS(X) \
    /* Core parsing keys */ \
    X(str_segment, "segment") \
    X(str_segments, "segments") \
    X(str_duration, "duration") \
    X(str_uri, "uri") \
    X(str_title, "title") \
    X(str_expect_segment, "expect_segment") \
    X(str_expect_playlist, "expect_playlist") \
    X(str_current_keys, "current_keys") \
    X(str_keys, "keys") \
    X(str_keyformat, "keyformat") \
    X(str_identity, "identity") \
    X(str_none_method, "NONE") \
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
    X(str_image_stream_info, "image_stream_info")

/*
 * Module state - holds all per-module data.
 *
 * Using module state instead of static globals ensures:
 * - Proper cleanup when the module is garbage collected
 * - Compatibility with subinterpreters (PEP 573, PEP 3121)
 * - Thread-safe access to cached objects
 */
typedef struct {
    PyObject *ParseError;
    PyObject *datetime_cls;
    PyObject *timedelta_cls;
    PyObject *fromisoformat_meth;
    /* Interned strings - generated from X-macro */
    #define DECLARE_INTERNED(name, str) PyObject *name;
    INTERNED_STRINGS(DECLARE_INTERNED)
    #undef DECLARE_INTERNED
} m3u8_state;

/*
 * Parse context - holds all state needed during a single parse() call.
 *
 * This structure reduces parameter passing between functions and makes
 * the parsing state explicit. All PyObject pointers in this struct are
 * borrowed references except where noted.
 *
 * Shadow State Optimization:
 * Hot flags (expect_segment, expect_playlist) are kept in C variables
 * to avoid dict lookup overhead in the main parsing loop. They are
 * synced to the Python state dict only when needed:
 * - Before calling custom_tags_parser (so callback sees current state)
 * - After custom_tags_parser returns (in case it modified state)
 * - At the end of parsing (for final state consistency)
 */
typedef struct {
    m3u8_state *mod_state;   /* Module state (borrowed) */
    PyObject *data;          /* Result dict being built (owned) */
    PyObject *state;         /* Parser state dict (owned) */
    int strict;              /* Strict parsing mode flag */
    int lineno;              /* Current line number (1-based) */
    /* Shadow state for hot flags - avoids dict lookups in main loop */
    int expect_segment;      /* Shadow of state["expect_segment"] */
    int expect_playlist;     /* Shadow of state["expect_playlist"] */
} ParseContext;

/*
 * Unified tag handler function type.
 *
 * All tag handlers receive the same arguments for consistency and to enable
 * the dispatch table pattern. This mirrors Python's **parse_kwargs approach.
 *
 * Args:
 *     ctx: Parse context (holds mod_state, data, state, strict, lineno)
 *     line: Null-terminated line content (the full line including tag)
 *
 * Returns:
 *     0 on success, -1 on failure with exception set
 */
typedef int (*TagHandler)(ParseContext *ctx, const char *line);

/*
 * Dispatch table entry for tag-to-handler mapping.
 *
 * Using a dispatch table instead of a long if/else chain:
 * - Matches Python's DISPATCH dict pattern
 * - More maintainable and readable
 * - Easier to add/remove tags
 * - Linear scan is fast for <50 tags (comparable to dict lookup overhead)
 */
typedef struct {
    const char *tag;      /* Tag string, e.g., "#EXTINF" */
    size_t tag_len;       /* Pre-computed length for fast prefix matching */
    TagHandler handler;   /* Handler function */
} TagDispatch;

/*
 * Sync shadow state TO Python dict (before custom_tags_parser or at end).
 */
static int
sync_shadow_to_dict(ParseContext *ctx)
{
    m3u8_state *mod_state = ctx->mod_state;
    if (dict_set_interned(ctx->state, mod_state->str_expect_segment,
                          ctx->expect_segment ? Py_True : Py_False) < 0) {
        return -1;
    }
    if (dict_set_interned(ctx->state, mod_state->str_expect_playlist,
                          ctx->expect_playlist ? Py_True : Py_False) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Sync shadow state FROM Python dict (after custom_tags_parser modifies it).
 */
static void
sync_shadow_from_dict(ParseContext *ctx)
{
    m3u8_state *mod_state = ctx->mod_state;
    PyObject *val;

    val = dict_get_interned(ctx->state, mod_state->str_expect_segment);
    ctx->expect_segment = (val == Py_True);

    val = dict_get_interned(ctx->state, mod_state->str_expect_playlist);
    ctx->expect_playlist = (val == Py_True);
}

/* Forward declaration for module definition */
static struct PyModuleDef m3u8_parser_module;

/* Get module state from module object */
static inline m3u8_state *
get_m3u8_state(PyObject *module)
{
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (m3u8_state *)state;
}

/*
 * Initialize datetime-related cached objects in module state.
 * Called during module initialization.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
init_datetime_cache(m3u8_state *state)
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
        Py_CLEAR(state->datetime_cls);
        Py_CLEAR(state->timedelta_cls);
        Py_CLEAR(state->fromisoformat_meth);
        return -1;
    }
    return 0;
}

/*
 * Initialize interned string cache using X-macro expansion.
 * Returns 0 on success, -1 on failure with exception set.
 */
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

/*
 * Raise ParseError with lineno and line arguments.
 * Takes module state to get the ParseError class.
 *
 * Optimization: Uses direct tuple construction instead of Py_BuildValue
 * to avoid format string parsing overhead.
 */
static void
raise_parse_error(m3u8_state *state, int lineno, const char *line)
{
    /* Direct tuple construction - faster than Py_BuildValue("(is)", ...) */
    PyObject *py_lineno = PyLong_FromLong(lineno);
    if (py_lineno == NULL) {
        return;
    }

    PyObject *py_line = PyUnicode_FromString(line);
    if (py_line == NULL) {
        Py_DECREF(py_lineno);
        return;
    }

    PyObject *args = PyTuple_Pack(2, py_lineno, py_line);
    Py_DECREF(py_lineno);
    Py_DECREF(py_line);
    if (args == NULL) {
        return;
    }

    PyObject *exc = PyObject_Call(state->ParseError, args, NULL);
    Py_DECREF(args);

    if (exc != NULL) {
        PyErr_SetObject(state->ParseError, exc);
        Py_DECREF(exc);
    }
}

/*
 * remove_quotes(), implemented at the Python level as:
 *
 *   quotes = ('"', "'")
 *   if string.startswith(quotes) and string.endswith(quotes):
 *       return string[1:-1]
 *
 * Note the subtlety: Python does NOT require matching quote characters.
 * We mirror that behavior for parity.
 *
 * Returns: new reference.
 */
static PyObject *
remove_quotes_py(PyObject *str)
{
    if (!PyUnicode_Check(str)) {
        PyErr_SetString(PyExc_TypeError, "expected str");
        return NULL;
    }

    Py_ssize_t len = PyUnicode_GetLength(str);
    if (len < 2) {
        return Py_NewRef(str);
    }

    Py_UCS4 first = PyUnicode_ReadChar(str, 0);
    if (first == (Py_UCS4)-1 && PyErr_Occurred()) {
        return NULL;
    }
    Py_UCS4 last = PyUnicode_ReadChar(str, len - 1);
    if (last == (Py_UCS4)-1 && PyErr_Occurred()) {
        return NULL;
    }

    if ((first == '"' || first == '\'') && (last == '"' || last == '\'')) {
        return PyUnicode_Substring(str, 1, len - 1);
    }
    return Py_NewRef(str);
}

/*
 * Strip leading and trailing whitespace from string in-place.
 *
 * Warning: This modifies the string in place by writing a NUL terminator.
 * Only use on mutable strings (e.g., our reusable line buffer).
 *
 * Returns pointer to first non-whitespace character (may be same as input
 * or point into the middle of the string).
 */
static char *strip(char *str) {
    while (ascii_isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    /* Safety: check length before computing end pointer to avoid UB */
    size_t len = strlen(str);
    if (len == 0) return str;
    char *end = str + len - 1;
    while (end > str && ascii_isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    return str;
}

/*
 * Fast dict operations using interned string keys.
 *
 * These avoid the string creation overhead of PyDict_SetItemString by
 * using pre-interned strings from module state. PyDict_SetItem with
 * interned strings can use pointer comparison for fast key lookup.
 */

/* Set dict[key] = value using interned key. Returns 0 on success, -1 on error. */
static inline int
dict_set_interned(PyObject *dict, PyObject *interned_key, PyObject *value)
{
    return PyDict_SetItem(dict, interned_key, value);
}

/* Get dict[key] using interned key. Returns borrowed ref or NULL. */
static inline PyObject *
dict_get_interned(PyObject *dict, PyObject *interned_key)
{
    return PyDict_GetItem(dict, interned_key);
}

/*
 * Get or create segment dict in state using interned string.
 * Returns borrowed reference on success, NULL with exception on failure.
 */
static PyObject *
get_or_create_segment(m3u8_state *mod_state, PyObject *state)
{
    PyObject *segment = dict_get_interned(state, mod_state->str_segment);
    if (segment != NULL) {
        return segment;  /* borrowed reference */
    }
    segment = PyDict_New();
    if (segment == NULL) {
        return NULL;
    }
    if (dict_set_interned(state, mod_state->str_segment, segment) < 0) {
        Py_DECREF(segment);
        return NULL;
    }
    Py_DECREF(segment);
    return dict_get_interned(state, mod_state->str_segment);
}

/* Utility: build list like Python's content.strip().splitlines() (preserve internal blanks) */
static PyObject *build_stripped_splitlines(const char *content) {
    const unsigned char *p = (const unsigned char *)content;
    const unsigned char *end = p + strlen(content);

    while (p < end && ascii_isspace(*p)) p++;
    while (end > p && ascii_isspace(*(end - 1))) end--;

    PyObject *lines = PyList_New(0);
    if (!lines) return NULL;

    const unsigned char *line_start = p;
    while (p < end) {
        if (*p == '\n' || *p == '\r') {
            PyObject *line = PyUnicode_FromStringAndSize((const char *)line_start,
                                                         (Py_ssize_t)(p - line_start));
            if (!line) {
                Py_DECREF(lines);
                return NULL;
            }
            if (PyList_Append(lines, line) < 0) {
                Py_DECREF(line);
                Py_DECREF(lines);
                return NULL;
            }
            Py_DECREF(line);

            /* Consume newline sequence */
            if (*p == '\r' && (p + 1) < end && *(p + 1) == '\n') p++;
            p++;
            line_start = p;
            continue;
        }
        p++;
    }

    /* Last line (even if empty) */
    PyObject *line = PyUnicode_FromStringAndSize((const char *)line_start,
                                                 (Py_ssize_t)(end - line_start));
    if (!line) {
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

/*
 * Helper to initialize multiple list fields using interned keys.
 * Returns 0 on success, -1 on failure.
 */
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

/*
 * Helper to initialize multiple dict fields using interned keys.
 * Returns 0 on success, -1 on failure.
 */
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

/*
 * Initialize the result data dictionary with default values.
 *
 * This sets up all the required keys with their initial values,
 * matching the structure created by the Python parser.
 *
 * Uses interned strings for faster dict operations (pointer comparison
 * instead of string hashing on each SetItem).
 *
 * Returns: New reference to data dict on success, NULL on failure.
 */
static PyObject *
init_parse_data(m3u8_state *ms)
{
    PyObject *data = PyDict_New();
    if (data == NULL) {
        return NULL;
    }

    /* Set scalar defaults using interned keys */
    PyObject *zero = PyLong_FromLong(0);
    if (zero == NULL) goto fail;
    if (dict_set_interned(data, ms->str_media_sequence, zero) < 0) {
        Py_DECREF(zero);
        goto fail;
    }
    Py_DECREF(zero);

    if (dict_set_interned(data, ms->str_is_variant, Py_False) < 0) goto fail;
    if (dict_set_interned(data, ms->str_is_endlist, Py_False) < 0) goto fail;
    if (dict_set_interned(data, ms->str_is_i_frames_only, Py_False) < 0) goto fail;
    if (dict_set_interned(data, ms->str_is_independent_segments, Py_False) < 0) goto fail;
    if (dict_set_interned(data, ms->str_is_images_only, Py_False) < 0) goto fail;
    if (dict_set_interned(data, ms->str_playlist_type, Py_None) < 0) goto fail;

    /* Initialize list fields using interned keys */
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

    /* Initialize dict fields using interned keys */
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
 * Initialize the parser state dictionary.
 *
 * The state dict tracks parsing progress and carries values between
 * tags (e.g., current key, segment being built, etc.).
 *
 * Returns: New reference to state dict on success, NULL on failure.
 */
static PyObject *
init_parse_state(m3u8_state *mod_state)
{
    PyObject *state = PyDict_New();
    if (state == NULL) {
        return NULL;
    }

    /* Use interned strings for commonly-accessed keys */
    if (dict_set_interned(state, mod_state->str_expect_segment, Py_False) < 0) goto fail;
    if (dict_set_interned(state, mod_state->str_expect_playlist, Py_False) < 0) goto fail;

    /* Initialize current_keys as empty list */
    PyObject *current_keys = PyList_New(0);
    if (current_keys == NULL) goto fail;
    if (dict_set_interned(state, mod_state->str_current_keys, current_keys) < 0) {
        Py_DECREF(current_keys);
        goto fail;
    }
    Py_DECREF(current_keys);

    return state;

fail:
    Py_DECREF(state);
    return NULL;
}

/*
 * Add seconds to a datetime object: dt + timedelta(seconds=secs)
 * Returns new reference on success, NULL with exception on failure.
 */
static PyObject *
datetime_add_seconds(m3u8_state *state, PyObject *dt, double secs)
{
    PyObject *args = PyTuple_New(0);
    if (args == NULL) {
        return NULL;
    }

    PyObject *kwargs = Py_BuildValue("{s:d}", "seconds", secs);
    if (kwargs == NULL) {
        Py_DECREF(args);
        return NULL;
    }

    PyObject *delta = PyObject_Call(state->timedelta_cls, args, kwargs);
    Py_DECREF(kwargs);
    Py_DECREF(args);
    if (delta == NULL) {
        return NULL;
    }

    PyObject *new_dt = PyNumber_Add(dt, delta);
    Py_DECREF(delta);
    return new_dt;
}

/*
 * Create normalized Python string directly from buffer (zero-copy optimization).
 *
 * This avoids malloc for keys < 64 chars (covers 99%+ of real-world cases).
 * Normalization: replace '-' with '_', lowercase, strip whitespace.
 *
 * Returns: New reference to Python string, or NULL with exception set.
 */
static PyObject *
create_normalized_key(const char *s, Py_ssize_t len)
{
    char stack_buf[64];
    char *buf = stack_buf;
    int use_heap = (len >= (Py_ssize_t)sizeof(stack_buf));

    if (use_heap) {
        buf = PyMem_Malloc(len + 1);
        if (buf == NULL) {
            return PyErr_NoMemory();
        }
    }

    /* Normalize: skip leading whitespace, replace - with _, tolower */
    Py_ssize_t in_idx = 0;
    Py_ssize_t out_len = 0;

    /* Skip leading whitespace */
    while (in_idx < len && ascii_isspace((unsigned char)s[in_idx])) {
        in_idx++;
    }

    /* Transform characters */
    for (; in_idx < len; in_idx++) {
        unsigned char c = (unsigned char)s[in_idx];
        if (c == '-') {
            c = '_';
        } else {
            c = ascii_tolower(c);
        }
        buf[out_len++] = (char)c;
    }

    /* Strip trailing whitespace */
    while (out_len > 0 && ascii_isspace((unsigned char)buf[out_len - 1])) {
        out_len--;
    }
    buf[out_len] = '\0';

    PyObject *res = PyUnicode_FromStringAndSize(buf, out_len);

    if (use_heap) {
        PyMem_Free(buf);
    }
    return res;
}

/* Utility: remove quotes from string */
/* Utility: delete a key from dict by interned key; ignore missing-key KeyError. */
static int
del_item_interned_ignore_keyerror(PyObject *dict, PyObject *interned_key)
{
    if (PyDict_DelItem(dict, interned_key) == 0) {
        return 0;
    }
    if (PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyErr_Clear();
        return 0;
    }
    return -1;
}

/*
 * Helper: Transfer boolean flag from state to segment.
 *
 * Sets segment[key] = True if state[key] exists, False otherwise.
 * Deletes state[key] if it existed.
 * Returns 0 on success, -1 on failure.
 */
static int
transfer_state_bool(PyObject *state, PyObject *segment, PyObject *key)
{
    PyObject *val = PyDict_GetItem(state, key);  /* borrowed ref, no error */
    if (PyDict_SetItem(segment, key, val ? Py_True : Py_False) < 0) return -1;
    if (val && del_item_interned_ignore_keyerror(state, key) < 0) return -1;
    return 0;
}

/*
 * Helper: Transfer value from state to segment (or None if missing).
 *
 * Sets segment[key] = state[key] if exists, else segment[key] = None.
 * Deletes state[key] if it existed.
 * Returns 0 on success, -1 on failure.
 */
static int
transfer_state_value(PyObject *state, PyObject *segment, PyObject *key)
{
    PyObject *val = PyDict_GetItem(state, key);  /* borrowed ref */
    if (PyDict_SetItem(segment, key, val ? val : Py_None) < 0) return -1;
    if (val && del_item_interned_ignore_keyerror(state, key) < 0) return -1;
    return 0;
}

/*
 * Zero-copy attribute list parser.
 *
 * Parses "KEY=value,KEY2=value2" format directly from buffer pointers.
 * Creates Python objects directly without intermediate C string allocations.
 *
 * Args:
 *     start: Pointer to start of attribute list (after the ":" in the tag)
 *     end: Pointer to end of buffer
 *
 * Returns: New reference to dict, or NULL with exception set.
 */
static PyObject *
parse_attribute_list_raw(const char *start, const char *end)
{
    PyObject *attrs = PyDict_New();
    if (attrs == NULL) {
        return NULL;
    }

    const char *p = start;
    while (p < end) {
        /* Skip leading whitespace and commas */
        while (p < end && (ascii_isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (p >= end) {
            break;
        }

        /* Find key */
        const char *key_start = p;
        while (p < end && *p != '=' && *p != ',') {
            p++;
        }
        const char *key_end = p;

        /* Create normalized key directly from buffer */
        PyObject *py_key = create_normalized_key(key_start, key_end - key_start);
        if (py_key == NULL) {
            Py_DECREF(attrs);
            return NULL;
        }

        PyObject *py_val = NULL;

        if (p < end && *p == '=') {
            p++;  /* Skip '=' */

            if (p < end && (*p == '"' || *p == '\'')) {
                /* Quoted string - include quotes in value for later processing */
                char quote = *p;
                const char *val_start = p;  /* Include opening quote */
                p++;  /* Skip opening quote */
                while (p < end && *p != quote) {
                    p++;
                }
                if (p < end) {
                    p++;  /* Include closing quote */
                }
                /* Create string with quotes (for compatibility with typed parser) */
                py_val = PyUnicode_FromStringAndSize(val_start, p - val_start);
            } else {
                /* Unquoted value */
                const char *val_start = p;
                while (p < end && *p != ',') {
                    p++;
                }
                /* Strip trailing whitespace from unquoted values */
                const char *val_end = p;
                while (val_end > val_start && ascii_isspace((unsigned char)*(val_end - 1))) {
                    val_end--;
                }
                py_val = PyUnicode_FromStringAndSize(val_start, val_end - val_start);
            }
        } else {
            /* Key without value - store the key content as value with empty key */
            /* This handles formats like "EXT-X-CUE-OUT-CONT:2.436/120" */
            Py_ssize_t key_len = key_end - key_start;
            /* Strip trailing whitespace */
            while (key_len > 0 && ascii_isspace((unsigned char)key_start[key_len - 1])) {
                key_len--;
            }
            py_val = PyUnicode_FromStringAndSize(key_start, key_len);
            Py_DECREF(py_key);
            py_key = PyUnicode_FromString("");
            if (py_key == NULL) {
                Py_XDECREF(py_val);
                Py_DECREF(attrs);
                return NULL;
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
    }

    return attrs;
}

/*
 * Parse attribute list from a line like "PREFIX:KEY=value,KEY2=value2"
 *
 * This is a wrapper around parse_attribute_list_raw that handles the
 * prefix-skipping logic for compatibility with existing callers.
 *
 * Returns new reference to dict on success, NULL with exception on failure.
 */
static PyObject *
parse_attribute_list(const char *line, const char *prefix)
{
    /* Skip prefix if present */
    const char *content = line;
    if (prefix != NULL) {
        size_t prefix_len = strlen(prefix);
        if (strncmp(line, prefix, prefix_len) == 0) {
            content = line + prefix_len;
            if (*content == ':') {
                content++;
            }
        } else {
            /* Prefix not found - return empty dict */
            return PyDict_New();
        }
    }

    /* Delegate to zero-copy implementation */
    return parse_attribute_list_raw(content, content + strlen(content));
}

/* Parse a key/value attribute list with type conversion */
typedef enum {
    ATTR_STRING,
    ATTR_INT,
    ATTR_FLOAT,
    ATTR_QUOTED_STRING,
    ATTR_BANDWIDTH
} AttrType;

typedef struct {
    const char *name;
    AttrType type;
} AttrParser;

/*
 * Schema-aware attribute parser.
 *
 * This is the optimized version that converts values to their final types
 * directly during parsing, avoiding the "double allocation" problem where
 * we first create a Python string, then convert it to int/float.
 *
 * The schema (parsers array) tells us the expected type for each key,
 * so we can parse directly to the correct Python type.
 *
 * Args:
 *     start: Pointer to start of attribute list (after "TAG:")
 *     end: Pointer to end of content
 *     parsers: Array of AttrParser structs defining key->type mappings
 *     num_parsers: Number of parsers in array
 *
 * Returns: New reference to dict on success, NULL with exception set.
 */
static PyObject *
parse_attributes_with_schema(const char *start, const char *end,
                             const AttrParser *parsers, size_t num_parsers)
{
    PyObject *attrs = PyDict_New();
    if (attrs == NULL) {
        return NULL;
    }

    const char *p = start;
    while (p < end) {
        /* Skip leading whitespace and commas */
        while (p < end && (ascii_isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (p >= end) {
            break;
        }

        /* Find key */
        const char *key_start = p;
        while (p < end && *p != '=' && *p != ',') {
            p++;
        }
        const char *key_end = p;
        size_t key_len = key_end - key_start;

        /* Determine type via schema lookup BEFORE creating Python objects */
        AttrType type = ATTR_STRING;
        if (parsers != NULL) {
            for (size_t i = 0; i < num_parsers; i++) {
                if (buffer_matches_key(key_start, key_len, parsers[i].name)) {
                    type = parsers[i].type;
                    break;
                }
            }
        }

        /* Create normalized Python key only once, after schema lookup */
        PyObject *py_key = create_normalized_key(key_start, key_len);
        if (py_key == NULL) {
            Py_DECREF(attrs);
            return NULL;
        }

        PyObject *py_val = NULL;

        if (p < end && *p == '=') {
            p++;  /* Skip '=' */

            if (p < end && (*p == '"' || *p == '\'')) {
                /* Quoted value */
                char quote = *p++;
                const char *full_start = p - 1;  /* include opening quote */
                const char *val_start = p;       /* inside quotes */
                while (p < end && *p != quote) {
                    p++;
                }
                const char *val_end = p;         /* points at closing quote or end */
                int has_closing_quote = (p < end && *p == quote);
                Py_ssize_t val_len = val_end - val_start;
                if (has_closing_quote) {
                    p++;  /* Skip closing quote */
                }

                /*
                 * Python parity:
                 * - Known "quoted string" attributes use remove_quotes() => no quotes
                 * - Unknown attributes keep the original token (including quotes)
                 */
                if (type == ATTR_QUOTED_STRING) {
                    py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                } else if (type == ATTR_STRING) {
                    Py_ssize_t full_len = has_closing_quote
                        ? (Py_ssize_t)((val_end - full_start) + 1)
                        : (Py_ssize_t)(val_end - full_start);
                    py_val = PyUnicode_FromStringAndSize(full_start, full_len);
                } else if (type == ATTR_INT || type == ATTR_BANDWIDTH) {
                    /* Numeric inside quotes - parse directly */
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        if (type == ATTR_BANDWIDTH) {
                            double v = PyOS_string_to_double(num_buf, NULL, NULL);
                            if (v == -1.0 && PyErr_Occurred()) {
                                PyErr_Clear();
                            } else {
                                py_val = PyLong_FromDouble(v);
                            }
                        } else {
                            py_val = PyLong_FromString(num_buf, NULL, 10);
                            if (py_val == NULL) {
                                PyErr_Clear();
                            }
                        }
                    }
                    /* Fallback to string if conversion fails */
                    if (py_val == NULL) {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else if (type == ATTR_FLOAT) {
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        double v = PyOS_string_to_double(num_buf, NULL, NULL);
                        if (v == -1.0 && PyErr_Occurred()) {
                            PyErr_Clear();
                            py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                        } else {
                            py_val = PyFloat_FromDouble(v);
                        }
                    } else {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else {
                    py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                }
            } else {
                /* Unquoted value */
                const char *val_start = p;
                while (p < end && *p != ',') {
                    p++;
                }
                /* Strip trailing whitespace */
                const char *val_end = p;
                while (val_end > val_start && ascii_isspace((unsigned char)*(val_end - 1))) {
                    val_end--;
                }
                Py_ssize_t val_len = val_end - val_start;

                /* Direct type conversion - no intermediate Python string! */
                if (type == ATTR_INT) {
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        py_val = PyLong_FromString(num_buf, NULL, 10);
                        if (py_val == NULL) {
                            PyErr_Clear();
                        }
                    }
                    if (py_val == NULL) {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else if (type == ATTR_BANDWIDTH) {
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        double v = PyOS_string_to_double(num_buf, NULL, NULL);
                        if (v == -1.0 && PyErr_Occurred()) {
                            PyErr_Clear();
                        } else {
                            py_val = PyLong_FromDouble(v);
                        }
                    }
                    if (py_val == NULL) {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else if (type == ATTR_FLOAT) {
                    char num_buf[64];
                    if (val_len < (Py_ssize_t)sizeof(num_buf)) {
                        memcpy(num_buf, val_start, val_len);
                        num_buf[val_len] = '\0';
                        double v = PyOS_string_to_double(num_buf, NULL, NULL);
                        if (v == -1.0 && PyErr_Occurred()) {
                            PyErr_Clear();
                            py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                        } else {
                            py_val = PyFloat_FromDouble(v);
                        }
                    } else {
                        py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                    }
                } else {
                    /* ATTR_STRING or ATTR_QUOTED_STRING (unquoted case) */
                    py_val = PyUnicode_FromStringAndSize(val_start, val_len);
                }
            }
        } else {
            /* Key without value - store key content as value with empty key */
            Py_ssize_t key_len = key_end - key_start;
            while (key_len > 0 && ascii_isspace((unsigned char)key_start[key_len - 1])) {
                key_len--;
            }
            py_val = PyUnicode_FromStringAndSize(key_start, key_len);
            Py_DECREF(py_key);
            py_key = PyUnicode_FromString("");
            if (py_key == NULL) {
                Py_XDECREF(py_val);
                Py_DECREF(attrs);
                return NULL;
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
    }

    return attrs;
}

/*
 * Wrapper for parse_attributes_with_schema that handles prefix skipping.
 * This maintains backward compatibility with existing callers.
 */
static PyObject *parse_typed_attribute_list(const char *line, const char *prefix,
                                            const AttrParser *parsers, size_t num_parsers) {
    /* Skip prefix if present */
    const char *content = line;
    if (prefix != NULL) {
        size_t prefix_len = strlen(prefix);
        if (strncmp(line, prefix, prefix_len) == 0) {
            content = line + prefix_len;
            if (*content == ':') {
                content++;
            }
        } else {
            /* Prefix not found - return empty dict */
            return PyDict_New();
        }
    }

    /* Delegate to schema-aware parser */
    return parse_attributes_with_schema(content, content + strlen(content),
                                        parsers, num_parsers);
}

/* Stream info attribute parsers */
static const AttrParser stream_inf_parsers[] = {
    {"codecs", ATTR_QUOTED_STRING},
    {"audio", ATTR_QUOTED_STRING},
    {"video", ATTR_QUOTED_STRING},
    {"video_range", ATTR_QUOTED_STRING},
    {"subtitles", ATTR_QUOTED_STRING},
    {"pathway_id", ATTR_QUOTED_STRING},
    {"stable_variant_id", ATTR_QUOTED_STRING},
    {"program_id", ATTR_INT},
    {"bandwidth", ATTR_BANDWIDTH},
    {"average_bandwidth", ATTR_INT},
    {"frame_rate", ATTR_FLOAT},
    {"hdcp_level", ATTR_STRING},
};
#define NUM_STREAM_INF_PARSERS (sizeof(stream_inf_parsers) / sizeof(stream_inf_parsers[0]))

/* Media attribute parsers */
static const AttrParser media_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"group_id", ATTR_QUOTED_STRING},
    {"language", ATTR_QUOTED_STRING},
    {"assoc_language", ATTR_QUOTED_STRING},
    {"name", ATTR_QUOTED_STRING},
    {"instream_id", ATTR_QUOTED_STRING},
    {"characteristics", ATTR_QUOTED_STRING},
    {"channels", ATTR_QUOTED_STRING},
    {"stable_rendition_id", ATTR_QUOTED_STRING},
    {"thumbnails", ATTR_QUOTED_STRING},
    {"image", ATTR_QUOTED_STRING},
};
#define NUM_MEDIA_PARSERS (sizeof(media_parsers) / sizeof(media_parsers[0]))

/* Part attribute parsers */
static const AttrParser part_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"duration", ATTR_FLOAT},
    {"independent", ATTR_STRING},
    {"gap", ATTR_STRING},
    {"byterange", ATTR_STRING},
};
#define NUM_PART_PARSERS (sizeof(part_parsers) / sizeof(part_parsers[0]))

/* Rendition report parsers */
static const AttrParser rendition_report_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"last_msn", ATTR_INT},
    {"last_part", ATTR_INT},
};
#define NUM_RENDITION_REPORT_PARSERS (sizeof(rendition_report_parsers) / sizeof(rendition_report_parsers[0]))

/* Skip parsers */
static const AttrParser skip_parsers[] = {
    {"recently_removed_dateranges", ATTR_QUOTED_STRING},
    {"skipped_segments", ATTR_INT},
};
#define NUM_SKIP_PARSERS (sizeof(skip_parsers) / sizeof(skip_parsers[0]))

/* Server control parsers */
static const AttrParser server_control_parsers[] = {
    {"can_block_reload", ATTR_STRING},
    {"hold_back", ATTR_FLOAT},
    {"part_hold_back", ATTR_FLOAT},
    {"can_skip_until", ATTR_FLOAT},
    {"can_skip_dateranges", ATTR_STRING},
};
#define NUM_SERVER_CONTROL_PARSERS (sizeof(server_control_parsers) / sizeof(server_control_parsers[0]))

/* Part inf parsers */
static const AttrParser part_inf_parsers[] = {
    {"part_target", ATTR_FLOAT},
};
#define NUM_PART_INF_PARSERS (sizeof(part_inf_parsers) / sizeof(part_inf_parsers[0]))

/* Preload hint parsers */
static const AttrParser preload_hint_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"type", ATTR_STRING},
    {"byterange_start", ATTR_INT},
    {"byterange_length", ATTR_INT},
};
#define NUM_PRELOAD_HINT_PARSERS (sizeof(preload_hint_parsers) / sizeof(preload_hint_parsers[0]))

/* Daterange parsers */
static const AttrParser daterange_parsers[] = {
    {"id", ATTR_QUOTED_STRING},
    {"class", ATTR_QUOTED_STRING},
    {"start_date", ATTR_QUOTED_STRING},
    {"end_date", ATTR_QUOTED_STRING},
    {"duration", ATTR_FLOAT},
    {"planned_duration", ATTR_FLOAT},
    {"end_on_next", ATTR_STRING},
    {"scte35_cmd", ATTR_STRING},
    {"scte35_out", ATTR_STRING},
    {"scte35_in", ATTR_STRING},
};
#define NUM_DATERANGE_PARSERS (sizeof(daterange_parsers) / sizeof(daterange_parsers[0]))

/* Session data parsers */
static const AttrParser session_data_parsers[] = {
    {"data_id", ATTR_QUOTED_STRING},
    {"value", ATTR_QUOTED_STRING},
    {"uri", ATTR_QUOTED_STRING},
    {"language", ATTR_QUOTED_STRING},
};
#define NUM_SESSION_DATA_PARSERS (sizeof(session_data_parsers) / sizeof(session_data_parsers[0]))

/* Content steering parsers */
static const AttrParser content_steering_parsers[] = {
    {"server_uri", ATTR_QUOTED_STRING},
    {"pathway_id", ATTR_QUOTED_STRING},
};
#define NUM_CONTENT_STEERING_PARSERS (sizeof(content_steering_parsers) / sizeof(content_steering_parsers[0]))

/* X-MAP parsers */
static const AttrParser x_map_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"byterange", ATTR_QUOTED_STRING},
};
#define NUM_X_MAP_PARSERS (sizeof(x_map_parsers) / sizeof(x_map_parsers[0]))

/* Start parsers */
static const AttrParser start_parsers[] = {
    {"time_offset", ATTR_FLOAT},
};
#define NUM_START_PARSERS (sizeof(start_parsers) / sizeof(start_parsers[0]))

/* Tiles parsers */
static const AttrParser tiles_parsers[] = {
    {"uri", ATTR_QUOTED_STRING},
    {"resolution", ATTR_STRING},
    {"layout", ATTR_STRING},
    {"duration", ATTR_FLOAT},
};
#define NUM_TILES_PARSERS (sizeof(tiles_parsers) / sizeof(tiles_parsers[0]))

/* Image stream inf parsers */
static const AttrParser image_stream_inf_parsers[] = {
    {"codecs", ATTR_QUOTED_STRING},
    {"uri", ATTR_QUOTED_STRING},
    {"pathway_id", ATTR_QUOTED_STRING},
    {"stable_variant_id", ATTR_QUOTED_STRING},
    {"program_id", ATTR_INT},
    {"bandwidth", ATTR_INT},
    {"average_bandwidth", ATTR_INT},
    {"resolution", ATTR_STRING},
};
#define NUM_IMAGE_STREAM_INF_PARSERS (sizeof(image_stream_inf_parsers) / sizeof(image_stream_inf_parsers[0]))

/* IFrame stream inf parsers */
static const AttrParser iframe_stream_inf_parsers[] = {
    {"codecs", ATTR_QUOTED_STRING},
    {"uri", ATTR_QUOTED_STRING},
    {"pathway_id", ATTR_QUOTED_STRING},
    {"stable_variant_id", ATTR_QUOTED_STRING},
    {"program_id", ATTR_INT},
    {"bandwidth", ATTR_INT},
    {"average_bandwidth", ATTR_INT},
    {"hdcp_level", ATTR_STRING},
};
#define NUM_IFRAME_STREAM_INF_PARSERS (sizeof(iframe_stream_inf_parsers) / sizeof(iframe_stream_inf_parsers[0]))

/* Cueout cont parsers */
static const AttrParser cueout_cont_parsers[] = {
    {"duration", ATTR_QUOTED_STRING},
    {"elapsedtime", ATTR_QUOTED_STRING},
    {"scte35", ATTR_QUOTED_STRING},
};
#define NUM_CUEOUT_CONT_PARSERS (sizeof(cueout_cont_parsers) / sizeof(cueout_cont_parsers[0]))

/* Cueout parsers */
static const AttrParser cueout_parsers[] = {
    {"cue", ATTR_QUOTED_STRING},
};
#define NUM_CUEOUT_PARSERS (sizeof(cueout_parsers) / sizeof(cueout_parsers[0]))


/*
 * Helper: parse attribute list with quote removal.
 * Returns new dict with unquoted values, or NULL on error.
 */
static PyObject *
parse_attrs_unquoted(const char *line, const char *tag)
{
    PyObject *raw_attrs = parse_attribute_list(line, tag);
    if (!raw_attrs) return NULL;

    PyObject *result = PyDict_New();
    if (!result) { Py_DECREF(raw_attrs); return NULL; }

    PyObject *k, *v;
    Py_ssize_t pos = 0;
    while (PyDict_Next(raw_attrs, &pos, &k, &v)) {
        PyObject *unquoted = remove_quotes_py(v);
        if (unquoted == NULL || PyDict_SetItem(result, k, unquoted) < 0) {
            Py_XDECREF(unquoted);
            Py_DECREF(result);
            Py_DECREF(raw_attrs);
            return NULL;
        }
        Py_DECREF(unquoted);
    }
    Py_DECREF(raw_attrs);
    return result;
}

/*
 * Helper: Compare two keyformat values for equality.
 * NULL or missing keyformat is treated as "identity".
 * Returns 1 if equal, 0 if not equal, -1 on error.
 */
static int
keyformat_equal(m3u8_state *mod_state, PyObject *kf1, PyObject *kf2)
{
    /* Both NULL/missing -> both "identity" -> equal */
    if (!kf1 && !kf2) return 1;

    /* One NULL/missing -> compare with "identity" */
    if (!kf1) {
        return PyObject_RichCompareBool(mod_state->str_identity, kf2, Py_EQ);
    }
    if (!kf2) {
        return PyObject_RichCompareBool(kf1, mod_state->str_identity, Py_EQ);
    }

    /* Both present -> compare directly */
    return PyObject_RichCompareBool(kf1, kf2, Py_EQ);
}

/* Parse a key tag */
static int
parse_key(m3u8_state *mod_state, const char *line, PyObject *data, PyObject *state)
{
    PyObject *key = parse_attrs_unquoted(line, EXT_X_KEY);
    if (!key) return -1;

    /* Get current_keys list from state */
    PyObject *current_keys = dict_get_interned(state, mod_state->str_current_keys);
    if (!current_keys) {
        Py_DECREF(key);
        return -1;
    }

    /* Get KEYFORMAT from the new key (defaults to "identity" if missing) */
    /* Note: METHOD=NONE is handled like any other key - it replaces keys with
     * the same KEYFORMAT. This distinguishes "explicitly unencrypted" from
     * "never had a key". */
    PyObject *new_keyformat = PyDict_GetItemString(key, "keyformat");  /* borrowed */

    /* Look for existing key with same KEYFORMAT and replace it */
    int replaced = 0;
    Py_ssize_t n = PyList_Size(current_keys);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *existing = PyList_GetItem(current_keys, i);  /* borrowed */
        PyObject *existing_keyformat = PyDict_GetItemString(existing, "keyformat");  /* borrowed */

        int eq = keyformat_equal(mod_state, new_keyformat, existing_keyformat);
        if (eq < 0) {
            Py_DECREF(key);
            return -1;
        }
        if (eq) {
            /* Replace existing key with same KEYFORMAT */
            if (PyList_SetItem(current_keys, i, Py_NewRef(key)) < 0) {
                Py_DECREF(key);
                return -1;
            }
            replaced = 1;
            break;
        }
    }

    /* If not replaced, append the new key */
    if (!replaced) {
        if (PyList_Append(current_keys, key) < 0) {
            Py_DECREF(key);
            return -1;
        }
    }

    /* Add to keys list if not already present */
    PyObject *keys = dict_get_interned(data, mod_state->str_keys);
    if (keys) {
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
    }

    Py_DECREF(key);
    return 0;
}

/*
 * Parse #EXTINF tag.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
parse_extinf(m3u8_state *mod_state, const char *line, PyObject *state,
             int lineno, int strict)
{
    const char *content = line + strlen(EXTINF) + 1;  /* Skip "#EXTINF:" */

    /* Find comma separator */
    const char *comma = strchr(content, ',');
    double duration;
    const char *title = "";

    if (comma != NULL) {
        char duration_str[64];
        size_t dur_len = comma - content;
        if (dur_len >= sizeof(duration_str)) {
            dur_len = sizeof(duration_str) - 1;
        }
        memcpy(duration_str, content, dur_len);
        duration_str[dur_len] = '\0';
        duration = PyOS_string_to_double(duration_str, NULL, NULL);
        if (duration == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            duration = 0.0;
        }
        title = comma + 1;
    } else {
        if (strict) {
            raise_parse_error(mod_state, lineno, line);
            return -1;
        }
        duration = PyOS_string_to_double(content, NULL, NULL);
        if (duration == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            duration = 0.0;
        }
    }

    /* Get or create segment dict in state using interned string */
    PyObject *segment = get_or_create_segment(mod_state, state);
    if (segment == NULL) {
        return -1;
    }

    /* Set duration using interned key */
    PyObject *py_duration = PyFloat_FromDouble(duration);
    if (py_duration == NULL) {
        return -1;
    }
    if (dict_set_interned(segment, mod_state->str_duration, py_duration) < 0) {
        Py_DECREF(py_duration);
        return -1;
    }
    Py_DECREF(py_duration);

    /* Set title using interned key */
    PyObject *py_title = PyUnicode_FromString(title);
    if (py_title == NULL) {
        return -1;
    }
    if (dict_set_interned(segment, mod_state->str_title, py_title) < 0) {
        Py_DECREF(py_title);
        return -1;
    }
    Py_DECREF(py_title);

    /* Set expect_segment flag using interned key */
    if (dict_set_interned(state, mod_state->str_expect_segment, Py_True) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Parse a segment URI line.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
parse_ts_chunk(m3u8_state *mod_state, const char *line,
               PyObject *data, PyObject *state)
{
    /* Get segment dict from state using interned key, or create new one */
    PyObject *segment = dict_get_interned(state, mod_state->str_segment);
    if (segment == NULL) {
        segment = PyDict_New();
        if (segment == NULL) {
            return -1;
        }
    } else {
        Py_INCREF(segment);
    }
    /* Remove segment from state (we're taking ownership) */
    if (PyDict_DelItem(state, mod_state->str_segment) < 0) {
        if (!PyErr_ExceptionMatches(PyExc_KeyError)) {
            Py_DECREF(segment);
            return -1;
        }
        PyErr_Clear();
    }

    /* Add URI using interned key */
    PyObject *uri = PyUnicode_FromString(line);
    if (uri == NULL) {
        Py_DECREF(segment);
        return -1;
    }
    if (dict_set_interned(segment, mod_state->str_uri, uri) < 0) {
        Py_DECREF(uri);
        Py_DECREF(segment);
        return -1;
    }
    Py_DECREF(uri);

    /* Transfer state values to segment (borrowed references) */
    PyObject *pdt = dict_get_interned(state, mod_state->str_program_date_time);
    if (pdt != NULL) {
        if (dict_set_interned(segment, mod_state->str_program_date_time, pdt) < 0) {
            Py_DECREF(segment);
            return -1;
        }
        if (del_item_interned_ignore_keyerror(state, mod_state->str_program_date_time) < 0) {
            Py_DECREF(segment);
            return -1;
        }
    }

    PyObject *current_pdt = dict_get_interned(state, mod_state->str_current_program_date_time);
    if (current_pdt != NULL) {
        if (dict_set_interned(segment, mod_state->str_current_program_date_time, current_pdt) < 0) {
            Py_DECREF(segment);
            return -1;
        }
        /* Update current_program_date_time by adding duration */
        PyObject *duration = dict_get_interned(segment, mod_state->str_duration);
        if (duration != NULL && current_pdt != NULL) {
            double secs = PyFloat_AsDouble(duration);
            if (PyErr_Occurred()) {
                Py_DECREF(segment);
                return -1;
            }
            PyObject *new_pdt = datetime_add_seconds(mod_state, current_pdt, secs);
            if (new_pdt == NULL) {
                Py_DECREF(segment);
                return -1;
            }
            if (dict_set_interned(state, mod_state->str_current_program_date_time, new_pdt) < 0) {
                Py_DECREF(new_pdt);
                Py_DECREF(segment);
                return -1;
            }
            Py_DECREF(new_pdt);
        }
    }

    /* Boolean flags from state - use transfer_state_bool helper */
    if (transfer_state_bool(state, segment, mod_state->str_cue_in) < 0) {
        Py_DECREF(segment);
        return -1;
    }

    /* cue_out needs special handling: check truthiness and keep state ref */
    PyObject *cue_out = dict_get_interned(state, mod_state->str_cue_out);
    int cue_out_truth = cue_out ? PyObject_IsTrue(cue_out) : 0;
    if (cue_out_truth < 0) {
        Py_DECREF(segment);
        return -1;
    }
    if (dict_set_interned(segment, mod_state->str_cue_out, cue_out_truth ? Py_True : Py_False) < 0) {
        Py_DECREF(segment);
        return -1;
    }

    if (transfer_state_bool(state, segment, mod_state->str_cue_out_start) < 0 ||
        transfer_state_bool(state, segment, mod_state->str_cue_out_explicitly_duration) < 0) {
        Py_DECREF(segment);
        return -1;
    }

    /* SCTE35 values - get if cue_out, pop otherwise */
    PyObject *scte_keys[] = {
        mod_state->str_current_cue_out_scte35,
        mod_state->str_current_cue_out_oatcls_scte35,
        mod_state->str_current_cue_out_duration,
        mod_state->str_current_cue_out_elapsedtime,
        mod_state->str_asset_metadata,
    };
    PyObject *seg_keys[] = {
        mod_state->str_scte35,
        mod_state->str_oatcls_scte35,
        mod_state->str_scte35_duration,
        mod_state->str_scte35_elapsedtime,
        mod_state->str_asset_metadata,
    };

    for (int i = 0; i < 5; i++) {
        PyObject *val = dict_get_interned(state, scte_keys[i]);
        if (val) {
            if (dict_set_interned(segment, seg_keys[i], val) < 0) {
                Py_DECREF(segment);
                return -1;
            }
            if (!cue_out_truth) {
                if (del_item_interned_ignore_keyerror(state, scte_keys[i]) < 0) {
                    Py_DECREF(segment);
                    return -1;
                }
            }
        } else {
            /* Clear any potential error from GetItem (though unlikely) */
            PyErr_Clear();
            if (dict_set_interned(segment, seg_keys[i], Py_None) < 0) {
                Py_DECREF(segment);
                return -1;
            }
        }
    }

    if (del_item_interned_ignore_keyerror(state, mod_state->str_cue_out) < 0) {
        Py_DECREF(segment);
        return -1;
    }

    /* Discontinuity */
    if (transfer_state_bool(state, segment, mod_state->str_discontinuity) < 0) {
        Py_DECREF(segment);
        return -1;
    }

    /* Keys - copy current_keys list to segment if non-empty */
    PyObject *current_keys = dict_get_interned(state, mod_state->str_current_keys);
    if (current_keys && PyList_Size(current_keys) > 0) {
        /* Create a shallow copy of the list */
        PyObject *keys_copy = PyList_GetSlice(current_keys, 0, PyList_Size(current_keys));
        if (!keys_copy) {
            Py_DECREF(segment);
            return -1;
        }
        if (dict_set_interned(segment, mod_state->str_keys, keys_copy) < 0) {
            Py_DECREF(keys_copy);
            Py_DECREF(segment);
            return -1;
        }
        Py_DECREF(keys_copy);
    } else {
        /* For unencrypted segments, ensure None is in keys list */
        PyObject *keys = dict_get_interned(data, mod_state->str_keys);
        if (keys) {
            int has_none = 0;
            Py_ssize_t n = PyList_Size(keys);
            for (Py_ssize_t i = 0; i < n; i++) {
                if (PyList_GetItem(keys, i) == Py_None) {
                    has_none = 1;
                    break;
                }
            }
            if (!has_none) {
                PyList_Append(keys, Py_None);
            }
        }
    }

    /* Init section */
    PyObject *current_segment_map = dict_get_interned(state, mod_state->str_current_segment_map);
    /* Only set init_section if the map dict is non-empty (matches Python's truthiness check) */
    if (current_segment_map && PyDict_Size(current_segment_map) > 0) {
        if (dict_set_interned(segment, mod_state->str_init_section, current_segment_map) < 0) {
            Py_DECREF(segment);
            return -1;
        }
    }

    /* Dateranges and Blackout - transfer value or None */
    if (transfer_state_value(state, segment, mod_state->str_dateranges) < 0 ||
        transfer_state_value(state, segment, mod_state->str_blackout) < 0) {
        Py_DECREF(segment);
        return -1;
    }

    /* Gap - special: read str_gap, write to str_gap_tag as True/None */
    PyObject *gap = dict_get_interned(state, mod_state->str_gap);
    if (dict_set_interned(segment, mod_state->str_gap_tag, gap ? Py_True : Py_None) < 0) {
        Py_DECREF(segment);
        return -1;
    }
    if (gap && del_item_interned_ignore_keyerror(state, mod_state->str_gap) < 0) {
        Py_DECREF(segment);
        return -1;
    }

    /* Add to segments list using interned key */
    PyObject *segments = dict_get_interned(data, mod_state->str_segments);
    if (segments) {
        if (PyList_Append(segments, segment) < 0) {
            Py_DECREF(segment);
            return -1;
        }
    }

    /* Clear expect_segment flag using interned key */
    if (dict_set_interned(state, mod_state->str_expect_segment, Py_False) < 0) {
        Py_DECREF(segment);
        return -1;
    }
    Py_DECREF(segment);
    return 0;
}

/* Parse variant playlist - uses interned strings throughout */
static int parse_variant_playlist(m3u8_state *ms, const char *line,
                                  PyObject *data, PyObject *state) {
    PyObject *stream_info = dict_get_interned(state, ms->str_stream_info);
    if (!stream_info) {
        stream_info = PyDict_New();
        if (!stream_info) return -1;
    } else {
        Py_INCREF(stream_info);
    }
    if (del_item_interned_ignore_keyerror(state, ms->str_stream_info) < 0) {
        Py_DECREF(stream_info);
        return -1;
    }

    PyObject *playlist = PyDict_New();
    if (!playlist) {
        Py_DECREF(stream_info);
        return -1;
    }

    PyObject *uri = PyUnicode_FromString(line);
    if (!uri) {
        Py_DECREF(playlist);
        Py_DECREF(stream_info);
        return -1;
    }
    if (dict_set_interned(playlist, ms->str_uri, uri) < 0) {
        Py_DECREF(uri);
        Py_DECREF(playlist);
        Py_DECREF(stream_info);
        return -1;
    }
    Py_DECREF(uri);

    if (dict_set_interned(playlist, ms->str_stream_info, stream_info) < 0) {
        Py_DECREF(playlist);
        Py_DECREF(stream_info);
        return -1;
    }
    Py_DECREF(stream_info);

    PyObject *playlists = dict_get_interned(data, ms->str_playlists);
    if (playlists && PyList_Append(playlists, playlist) < 0) {
        Py_DECREF(playlist);
        return -1;
    }
    Py_DECREF(playlist);

    return dict_set_interned(state, ms->str_expect_playlist, Py_False);
}

/*
 * Parse EXT-X-PROGRAM-DATE-TIME tag - uses interned strings.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
parse_program_date_time(m3u8_state *ms, const char *line,
                        PyObject *data, PyObject *state)
{
    const char *value = strchr(line, ':');
    if (value == NULL) return 0;
    value++;

    PyObject *dt = PyObject_CallFunction(ms->fromisoformat_meth, "s", value);
    if (dt == NULL) return -1;

    /* Set in data if not already set */
    PyObject *existing = dict_get_interned(data, ms->str_program_date_time);
    if (existing == NULL || existing == Py_None) {
        if (dict_set_interned(data, ms->str_program_date_time, dt) < 0) {
            Py_DECREF(dt);
            return -1;
        }
    }

    if (dict_set_interned(state, ms->str_current_program_date_time, dt) < 0 ||
        dict_set_interned(state, ms->str_program_date_time, dt) < 0) {
        Py_DECREF(dt);
        return -1;
    }
    Py_DECREF(dt);
    return 0;
}

/*
 * Parse EXT-X-PART tag - uses interned strings throughout.
 * Returns 0 on success, -1 on failure with exception set.
 */
static int
parse_part(m3u8_state *ms, const char *line, PyObject *state)
{
    PyObject *part = parse_typed_attribute_list(line, EXT_X_PART,
                                                 part_parsers, NUM_PART_PARSERS);
    if (part == NULL) return -1;

    /* Add program_date_time if available */
    PyObject *current_pdt = dict_get_interned(state, ms->str_current_program_date_time);
    if (current_pdt != NULL) {
        if (dict_set_interned(part, ms->str_program_date_time, current_pdt) < 0) {
            Py_DECREF(part);
            return -1;
        }
        /* Update current_program_date_time */
        PyObject *duration = dict_get_interned(part, ms->str_duration);
        if (duration != NULL) {
            double secs = PyFloat_AsDouble(duration);
            if (PyErr_Occurred()) {
                Py_DECREF(part);
                return -1;
            }
            PyObject *new_pdt = datetime_add_seconds(ms, current_pdt, secs);
            if (new_pdt == NULL) {
                Py_DECREF(part);
                return -1;
            }
            if (dict_set_interned(state, ms->str_current_program_date_time, new_pdt) < 0) {
                Py_DECREF(new_pdt);
                Py_DECREF(part);
                return -1;
            }
            Py_DECREF(new_pdt);
        }
    }

    /* Add dateranges - use transfer_state_value pattern */
    if (transfer_state_value(state, part, ms->str_dateranges) < 0) {
        Py_DECREF(part);
        return -1;
    }

    /* Add gap_tag - read from str_gap, write True/None to str_gap_tag */
    PyObject *gap = dict_get_interned(state, ms->str_gap);
    if (dict_set_interned(part, ms->str_gap_tag, gap ? Py_True : Py_None) < 0) {
        Py_DECREF(part);
        return -1;
    }
    if (gap && del_item_interned_ignore_keyerror(state, ms->str_gap) < 0) {
        Py_DECREF(part);
        return -1;
    }

    /* Get or create segment */
    PyObject *segment = dict_get_interned(state, ms->str_segment);
    if (segment == NULL) {
        segment = PyDict_New();
        if (segment == NULL) {
            Py_DECREF(part);
            return -1;
        }
        if (dict_set_interned(state, ms->str_segment, segment) < 0) {
            Py_DECREF(segment);
            Py_DECREF(part);
            return -1;
        }
        Py_DECREF(segment);
        segment = dict_get_interned(state, ms->str_segment);
    }

    /* Get or create parts list in segment */
    PyObject *parts = dict_get_interned(segment, ms->str_parts);
    if (parts == NULL) {
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
        Py_DECREF(parts);
        parts = dict_get_interned(segment, ms->str_parts);
    }

    if (PyList_Append(parts, part) < 0) {
        Py_DECREF(part);
        return -1;
    }
    Py_DECREF(part);
    return 0;
}

/* Parse cue out - uses interned strings for state dict */
static int parse_cueout(m3u8_state *ms, const char *line, PyObject *state) {
    if (dict_set_interned(state, ms->str_cue_out_start, Py_True) < 0 ||
        dict_set_interned(state, ms->str_cue_out, Py_True) < 0) {
        return -1;
    }

    /* Check for DURATION keyword */
    char upper_line[1024];
    size_t i;
    for (i = 0; i < sizeof(upper_line) - 1 && line[i]; i++) {
        upper_line[i] = toupper((unsigned char)line[i]);
    }
    upper_line[i] = '\0';

    if (strstr(upper_line, "DURATION")) {
        if (dict_set_interned(state, ms->str_cue_out_explicitly_duration, Py_True) < 0) {
            return -1;
        }
    }

    /* Parse attributes if present */
    const char *colon = strchr(line, ':');
    if (!colon || *(colon + 1) == '\0') {
        return 0;
    }

    PyObject *cue_info = parse_typed_attribute_list(line, EXT_X_CUE_OUT,
        cueout_parsers, NUM_CUEOUT_PARSERS);
    if (!cue_info) return -1;

    /* cue_info uses attr keys like "cue", "duration", "" - not interned */
    PyObject *cue_out_scte35 = PyDict_GetItemString(cue_info, "cue");
    PyObject *cue_out_duration = PyDict_GetItemString(cue_info, "duration");
    if (!cue_out_duration) {
        cue_out_duration = PyDict_GetItemString(cue_info, "");
    }

    /* State dict uses interned keys */
    if (cue_out_scte35) {
        if (dict_set_interned(state, ms->str_current_cue_out_scte35, cue_out_scte35) < 0) {
            Py_DECREF(cue_info);
            return -1;
        }
    }
    if (cue_out_duration) {
        if (dict_set_interned(state, ms->str_current_cue_out_duration, cue_out_duration) < 0) {
            Py_DECREF(cue_info);
            return -1;
        }
    }

    Py_DECREF(cue_info);
    return 0;
}

/* Parse cue out cont - uses interned strings for state dict */
static int parse_cueout_cont(m3u8_state *ms, const char *line, PyObject *state) {
    if (dict_set_interned(state, ms->str_cue_out, Py_True) < 0) return -1;

    const char *colon = strchr(line, ':');
    if (!colon || *(colon + 1) == '\0') return 0;

    PyObject *cue_info = parse_typed_attribute_list(line, EXT_X_CUE_OUT_CONT,
        cueout_cont_parsers, NUM_CUEOUT_CONT_PARSERS);
    if (!cue_info) return -1;

    /* cue_info uses attr keys like "", "duration", etc. - not interned */
    PyObject *progress = PyDict_GetItemString(cue_info, "");
    if (progress) {
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
            /* Use interned keys for state dict */
            int rc1 = dict_set_interned(state, ms->str_current_cue_out_elapsedtime, elapsed);
            int rc2 = dict_set_interned(state, ms->str_current_cue_out_duration, duration);
            Py_DECREF(elapsed);
            Py_DECREF(duration);
            if (rc1 < 0 || rc2 < 0) {
                Py_DECREF(cue_info);
                return -1;
            }
        } else {
            if (dict_set_interned(state, ms->str_current_cue_out_duration, progress) < 0) {
                Py_DECREF(cue_info);
                return -1;
            }
        }
    }

    PyObject *duration = PyDict_GetItemString(cue_info, "duration");
    if (duration && dict_set_interned(state, ms->str_current_cue_out_duration, duration) < 0) {
        Py_DECREF(cue_info);
        return -1;
    }

    PyObject *scte35 = PyDict_GetItemString(cue_info, "scte35");
    if (scte35 && dict_set_interned(state, ms->str_current_cue_out_scte35, scte35) < 0) {
        Py_DECREF(cue_info);
        return -1;
    }

    PyObject *elapsedtime = PyDict_GetItemString(cue_info, "elapsedtime");
    if (elapsedtime && dict_set_interned(state, ms->str_current_cue_out_elapsedtime, elapsedtime) < 0) {
        Py_DECREF(cue_info);
        return -1;
    }

    Py_DECREF(cue_info);
    return 0;
}

/*
 * ============================================================================
 * Dispatch Table Handlers
 *
 * These wrapper functions provide a unified signature for the dispatch table.
 * Each handler receives a ParseContext and the full line, extracts what it
 * needs, and calls the appropriate parsing logic.
 * ============================================================================
 */

/*
 * Macro-generated integer value handlers.
 * These handlers parse an integer value after the tag and store it.
 */
#define MAKE_INT_HANDLER(name, tag, field) \
    static int name(ParseContext *ctx, const char *line) { \
        const char *value = line + sizeof(tag); \
        PyObject *py_value = PyLong_FromString(value, NULL, 10); \
        if (py_value == NULL) { PyErr_Clear(); return 0; } \
        int rc = dict_set_interned(ctx->data, ctx->mod_state->field, py_value); \
        Py_DECREF(py_value); \
        return rc < 0 ? -1 : 0; \
    }

MAKE_INT_HANDLER(handle_targetduration, EXT_X_TARGETDURATION, str_targetduration)
MAKE_INT_HANDLER(handle_media_sequence, EXT_X_MEDIA_SEQUENCE, str_media_sequence)
MAKE_INT_HANDLER(handle_discontinuity_sequence, EXT_X_DISCONTINUITY_SEQUENCE, str_discontinuity_sequence)

/*
 * Macro-generated wrapper handlers that delegate to existing parse functions.
 */
#define MAKE_PARSE_WRAPPER(name, fn) \
    static int name(ParseContext *ctx, const char *line) { \
        return fn(ctx->mod_state, line, ctx->data, ctx->state); \
    }

MAKE_PARSE_WRAPPER(handle_program_date_time, parse_program_date_time)
MAKE_PARSE_WRAPPER(handle_key, parse_key)

#undef MAKE_PARSE_WRAPPER

/* Handler for #EXTINF */
static int
handle_extinf(ParseContext *ctx, const char *line)
{
    int rc = parse_extinf(ctx->mod_state, line, ctx->state, ctx->lineno, ctx->strict);
    if (rc == 0) {
        ctx->expect_segment = 1;
    }
    return rc;
}

/* Handler for #EXT-X-BYTERANGE */
static int
handle_byterange(ParseContext *ctx, const char *line)
{
    const char *value = line + sizeof(EXT_X_BYTERANGE);
    PyObject *segment = get_or_create_segment(ctx->mod_state, ctx->state);
    if (segment == NULL) {
        return -1;
    }
    PyObject *py_value = PyUnicode_FromString(value);
    if (py_value == NULL) {
        return -1;
    }
    int rc = dict_set_interned(segment, ctx->mod_state->str_byterange, py_value);
    Py_DECREF(py_value);
    if (rc < 0) {
        return -1;
    }
    ctx->expect_segment = 1;
    return dict_set_interned(ctx->state, ctx->mod_state->str_expect_segment, Py_True);
}

/* Handler for #EXT-X-BITRATE */
static int
handle_bitrate(ParseContext *ctx, const char *line)
{
    const char *value = line + sizeof(EXT_X_BITRATE);
    PyObject *segment = get_or_create_segment(ctx->mod_state, ctx->state);
    if (segment == NULL) {
        return -1;
    }
    PyObject *py_value = PyLong_FromString(value, NULL, 10);
    if (py_value == NULL) {
        PyErr_Clear();
        return 0;
    }
    int rc = dict_set_interned(segment, ctx->mod_state->str_bitrate, py_value);
    Py_DECREF(py_value);
    return rc < 0 ? -1 : 0;
}

/* Handler for #EXT-X-STREAM-INF */
static int
handle_stream_inf(ParseContext *ctx, const char *line)
{
    m3u8_state *ms = ctx->mod_state;
    /* Use shadow state only - synced to dict before custom parser/end of parse */
    ctx->expect_playlist = 1;
    if (dict_set_interned(ctx->data, ms->str_is_variant, Py_True) < 0) return -1;
    if (dict_set_interned(ctx->data, ms->str_media_sequence, Py_None) < 0) return -1;

    PyObject *stream_info = parse_typed_attribute_list(line, EXT_X_STREAM_INF,
        stream_inf_parsers, NUM_STREAM_INF_PARSERS);
    if (stream_info == NULL) return -1;
    int rc = dict_set_interned(ctx->state, ms->str_stream_info, stream_info);
    Py_DECREF(stream_info);
    return rc < 0 ? -1 : 0;
}

/*
 * Helper for iframe/image stream handlers - parses attrs, extracts URI,
 * builds playlist dict, and appends to list. Uses interned str_uri.
 */
static int
handle_stream_inf_with_uri(ParseContext *ctx, const char *line, const char *tag,
                           const AttrParser *parsers, size_t num_parsers,
                           const char *info_key, PyObject *list_key)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *info = parse_typed_attribute_list(line, tag, parsers, num_parsers);
    if (info == NULL) return -1;

    /* Use interned string for URI lookup */
    PyObject *uri = dict_get_interned(info, ms->str_uri);
    if (uri == NULL) { Py_DECREF(info); return 0; }

    Py_INCREF(uri);
    if (del_item_interned_ignore_keyerror(info, ms->str_uri) < 0) {
        Py_DECREF(uri);
        Py_DECREF(info);
        return -1;
    }

    /* info_key passed as char* - Py_BuildValue "s" is fine for rare keys */
    PyObject *playlist = Py_BuildValue("{s:N,s:N}", "uri", uri, info_key, info);
    if (playlist == NULL) return -1;

    PyObject *list = dict_get_interned(ctx->data, list_key);
    int rc = PyList_Append(list, playlist);
    Py_DECREF(playlist);
    return rc;
}

static int handle_i_frame_stream_inf(ParseContext *ctx, const char *line) {
    return handle_stream_inf_with_uri(ctx, line, EXT_X_I_FRAME_STREAM_INF,
        iframe_stream_inf_parsers, NUM_IFRAME_STREAM_INF_PARSERS,
        "iframe_stream_info", ctx->mod_state->str_iframe_playlists);
}

static int handle_image_stream_inf(ParseContext *ctx, const char *line) {
    return handle_stream_inf_with_uri(ctx, line, EXT_X_IMAGE_STREAM_INF,
        image_stream_inf_parsers, NUM_IMAGE_STREAM_INF_PARSERS,
        "image_stream_info", ctx->mod_state->str_image_playlists);
}

/*
 * Macro-generated handlers that parse typed attributes and append to a list.
 */
#define MAKE_TYPED_ATTR_LIST_HANDLER(name, tag, parsers, num_parsers, field) \
    static int name(ParseContext *ctx, const char *line) { \
        PyObject *result = parse_typed_attribute_list(line, tag, parsers, num_parsers); \
        if (result == NULL) return -1; \
        PyObject *list = dict_get_interned(ctx->data, ctx->mod_state->field); \
        int rc = PyList_Append(list, result); \
        Py_DECREF(result); \
        return rc; \
    }

MAKE_TYPED_ATTR_LIST_HANDLER(handle_media, EXT_X_MEDIA, media_parsers, NUM_MEDIA_PARSERS, str_media)

/* Handler for #EXT-X-PLAYLIST-TYPE */
static int
handle_playlist_type(ParseContext *ctx, const char *line)
{
    const char *value = line + sizeof(EXT_X_PLAYLIST_TYPE);
    /* Use create_normalized_key for safe, DRY normalization (tolower + strip) */
    PyObject *py_value = create_normalized_key(value, strlen(value));
    if (py_value == NULL) return -1;
    int rc = dict_set_interned(ctx->data, ctx->mod_state->str_playlist_type, py_value);
    Py_DECREF(py_value);
    return rc < 0 ? -1 : 0;
}

MAKE_INT_HANDLER(handle_version, EXT_X_VERSION, str_version)

#undef MAKE_INT_HANDLER

/* Handler for #EXT-X-ALLOW-CACHE */
static int
handle_allow_cache(ParseContext *ctx, const char *line)
{
    const char *value = line + sizeof(EXT_X_ALLOW_CACHE);
    char normalized[64];
    size_t i;
    for (i = 0; i < sizeof(normalized) - 1 && value[i]; i++) {
        normalized[i] = ascii_tolower((unsigned char)value[i]);
    }
    normalized[i] = '\0';
    PyObject *py_value = PyUnicode_FromString(strip(normalized));
    if (py_value == NULL) {
        return -1;
    }
    int rc = dict_set_interned(ctx->data, ctx->mod_state->str_allow_cache, py_value);
    Py_DECREF(py_value);
    return rc < 0 ? -1 : 0;
}

/*
 * Macro-generated flag handlers.
 * These handlers just set a boolean flag in either data or state dict.
 */
#define MAKE_DATA_FLAG_HANDLER(name, field) \
    static int name(ParseContext *ctx, const char *line) { \
        (void)line; \
        return dict_set_interned(ctx->data, ctx->mod_state->field, Py_True); \
    }

#define MAKE_STATE_FLAG_HANDLER(name, field) \
    static int name(ParseContext *ctx, const char *line) { \
        (void)line; \
        return dict_set_interned(ctx->state, ctx->mod_state->field, Py_True); \
    }

MAKE_DATA_FLAG_HANDLER(handle_i_frames_only, str_is_i_frames_only)
MAKE_DATA_FLAG_HANDLER(handle_independent_segments, str_is_independent_segments)
MAKE_DATA_FLAG_HANDLER(handle_endlist, str_is_endlist)
MAKE_DATA_FLAG_HANDLER(handle_images_only, str_is_images_only)
MAKE_STATE_FLAG_HANDLER(handle_discontinuity, str_discontinuity)
MAKE_STATE_FLAG_HANDLER(handle_cue_in, str_cue_in)
MAKE_STATE_FLAG_HANDLER(handle_cue_span, str_cue_out)
MAKE_STATE_FLAG_HANDLER(handle_gap, str_gap)

#undef MAKE_DATA_FLAG_HANDLER
#undef MAKE_STATE_FLAG_HANDLER

/* Wrapper handlers for cue parsing */
static int handle_cue_out(ParseContext *ctx, const char *line) {
    return parse_cueout(ctx->mod_state, line, ctx->state);
}
static int handle_cue_out_cont(ParseContext *ctx, const char *line) {
    return parse_cueout_cont(ctx->mod_state, line, ctx->state);
}

/* Handler for #EXT-OATCLS-SCTE35 - uses interned strings */
static int
handle_oatcls_scte35(ParseContext *ctx, const char *line)
{
    m3u8_state *ms = ctx->mod_state;
    const char *value = strchr(line, ':');
    if (value == NULL) return 0;
    value++;

    PyObject *py_value = PyUnicode_FromString(value);
    if (py_value == NULL) return -1;

    if (dict_set_interned(ctx->state, ms->str_current_cue_out_oatcls_scte35, py_value) < 0) {
        Py_DECREF(py_value);
        return -1;
    }
    PyObject *current = dict_get_interned(ctx->state, ms->str_current_cue_out_scte35);
    if (current == NULL) {
        if (dict_set_interned(ctx->state, ms->str_current_cue_out_scte35, py_value) < 0) {
            Py_DECREF(py_value);
            return -1;
        }
    }
    Py_DECREF(py_value);
    return 0;
}

/* Handler for #EXT-X-ASSET - uses interned strings */
static int
handle_asset(ParseContext *ctx, const char *line)
{
    PyObject *asset = parse_attribute_list(line, EXT_X_ASSET);
    if (asset == NULL) return -1;
    int rc = dict_set_interned(ctx->state, ctx->mod_state->str_asset_metadata, asset);
    Py_DECREF(asset);
    return rc < 0 ? -1 : 0;
}

/* Handler for #EXT-X-MAP */
static int
handle_map(ParseContext *ctx, const char *line)
{
    m3u8_state *ms = ctx->mod_state;
    PyObject *map_info = parse_typed_attribute_list(line, EXT_X_MAP,
        x_map_parsers, NUM_X_MAP_PARSERS);
    if (map_info == NULL) {
        return -1;
    }
    if (dict_set_interned(ctx->state, ms->str_current_segment_map, map_info) < 0) {
        Py_DECREF(map_info);
        return -1;
    }
    PyObject *segment_map = dict_get_interned(ctx->data, ms->str_segment_map);
    int rc = PyList_Append(segment_map, map_info);
    Py_DECREF(map_info);
    return rc;
}

/*
 * Macro-generated typed attribute handlers.
 * These parse typed attributes and store the result in ctx->data.
 */
#define MAKE_TYPED_ATTR_HANDLER(name, tag, parsers, num_parsers, field) \
    static int name(ParseContext *ctx, const char *line) { \
        PyObject *result = parse_typed_attribute_list(line, tag, parsers, num_parsers); \
        if (result == NULL) return -1; \
        int rc = dict_set_interned(ctx->data, ctx->mod_state->field, result); \
        Py_DECREF(result); \
        return rc < 0 ? -1 : 0; \
    }

MAKE_TYPED_ATTR_HANDLER(handle_start, EXT_X_START, start_parsers, NUM_START_PARSERS, str_start)
MAKE_TYPED_ATTR_HANDLER(handle_server_control, EXT_X_SERVER_CONTROL, server_control_parsers, NUM_SERVER_CONTROL_PARSERS, str_server_control)
MAKE_TYPED_ATTR_HANDLER(handle_part_inf, EXT_X_PART_INF, part_inf_parsers, NUM_PART_INF_PARSERS, str_part_inf)

/* Handler for #EXT-X-PART */
static int handle_part(ParseContext *ctx, const char *line) {
    return parse_part(ctx->mod_state, line, ctx->state);
}

MAKE_TYPED_ATTR_LIST_HANDLER(handle_rendition_report, EXT_X_RENDITION_REPORT, rendition_report_parsers, NUM_RENDITION_REPORT_PARSERS, str_rendition_reports)

MAKE_TYPED_ATTR_HANDLER(handle_skip, EXT_X_SKIP, skip_parsers, NUM_SKIP_PARSERS, str_skip)

MAKE_TYPED_ATTR_LIST_HANDLER(handle_session_data, EXT_X_SESSION_DATA, session_data_parsers, NUM_SESSION_DATA_PARSERS, str_session_data)
MAKE_TYPED_ATTR_LIST_HANDLER(handle_tiles, EXT_X_TILES, tiles_parsers, NUM_TILES_PARSERS, str_tiles)

#undef MAKE_TYPED_ATTR_LIST_HANDLER

/* Handler for #EXT-X-SESSION-KEY */
static int handle_session_key(ParseContext *ctx, const char *line) {
    PyObject *key = parse_attrs_unquoted(line, EXT_X_SESSION_KEY);
    if (!key) return -1;
    PyObject *session_keys = dict_get_interned(ctx->data, ctx->mod_state->str_session_keys);
    int rc = PyList_Append(session_keys, key);
    Py_DECREF(key);
    return rc;
}

MAKE_TYPED_ATTR_HANDLER(handle_preload_hint, EXT_X_PRELOAD_HINT, preload_hint_parsers, NUM_PRELOAD_HINT_PARSERS, str_preload_hint)

/* Handler for #EXT-X-DATERANGE */
static int
handle_daterange(ParseContext *ctx, const char *line)
{
    PyObject *daterange = parse_typed_attribute_list(line, EXT_X_DATERANGE,
        daterange_parsers, NUM_DATERANGE_PARSERS);
    if (daterange == NULL) {
        return -1;
    }

    PyObject *dateranges = dict_get_interned(ctx->state, ctx->mod_state->str_dateranges);
    if (dateranges == NULL) {
        dateranges = PyList_New(0);
        if (dateranges == NULL) {
            Py_DECREF(daterange);
            return -1;
        }
        if (dict_set_interned(ctx->state, ctx->mod_state->str_dateranges, dateranges) < 0) {
            Py_DECREF(dateranges);
            Py_DECREF(daterange);
            return -1;
        }
        Py_DECREF(dateranges);
        dateranges = dict_get_interned(ctx->state, ctx->mod_state->str_dateranges);
    }

    int rc = PyList_Append(dateranges, daterange);
    Py_DECREF(daterange);
    return rc;
}

MAKE_TYPED_ATTR_HANDLER(handle_content_steering, EXT_X_CONTENT_STEERING, content_steering_parsers, NUM_CONTENT_STEERING_PARSERS, str_content_steering)

#undef MAKE_TYPED_ATTR_HANDLER

/* Handler for #EXT-X-BLACKOUT */
static int
handle_blackout(ParseContext *ctx, const char *line)
{
    const char *colon = strchr(line, ':');
    if (colon != NULL && *(colon + 1) != '\0') {
        PyObject *blackout_data = PyUnicode_FromString(colon + 1);
        if (blackout_data == NULL) {
            return -1;
        }
        int rc = dict_set_interned(ctx->state, ctx->mod_state->str_blackout, blackout_data);
        Py_DECREF(blackout_data);
        return rc < 0 ? -1 : 0;
    }
    return dict_set_interned(ctx->state, ctx->mod_state->str_blackout, Py_True);
}

/*
 * Tag dispatch table.
 *
 * This replaces the massive if/else strcmp chain with a data-driven approach.
 * Linear scan for ~40 tags is negligible compared to Python object creation.
 * The table is ordered roughly by frequency for marginally better cache behavior.
 *
 * Note: sizeof(TAG)-1 gives strlen at compile time (excluding null terminator).
 */
static const TagDispatch TAG_DISPATCH[] = {
    /* High-frequency tags first */
    {EXTINF,                      sizeof(EXTINF)-1,                      handle_extinf},
    {EXT_X_KEY,                   sizeof(EXT_X_KEY)-1,                   handle_key},
    {EXT_X_BYTERANGE,             sizeof(EXT_X_BYTERANGE)-1,             handle_byterange},
    {EXT_X_PROGRAM_DATE_TIME,     sizeof(EXT_X_PROGRAM_DATE_TIME)-1,     handle_program_date_time},
    {EXT_X_DISCONTINUITY,         sizeof(EXT_X_DISCONTINUITY)-1,         handle_discontinuity},
    {EXT_X_MAP,                   sizeof(EXT_X_MAP)-1,                   handle_map},
    {EXT_X_PART,                  sizeof(EXT_X_PART)-1,                  handle_part},
    {EXT_X_BITRATE,               sizeof(EXT_X_BITRATE)-1,               handle_bitrate},
    {EXT_X_GAP,                   sizeof(EXT_X_GAP)-1,                   handle_gap},
    {EXT_X_DATERANGE,             sizeof(EXT_X_DATERANGE)-1,             handle_daterange},
    /* Variant playlist tags */
    {EXT_X_STREAM_INF,            sizeof(EXT_X_STREAM_INF)-1,            handle_stream_inf},
    {EXT_X_MEDIA,                 sizeof(EXT_X_MEDIA)-1,                 handle_media},
    {EXT_X_I_FRAME_STREAM_INF,    sizeof(EXT_X_I_FRAME_STREAM_INF)-1,    handle_i_frame_stream_inf},
    {EXT_X_IMAGE_STREAM_INF,      sizeof(EXT_X_IMAGE_STREAM_INF)-1,      handle_image_stream_inf},
    {EXT_X_SESSION_DATA,          sizeof(EXT_X_SESSION_DATA)-1,          handle_session_data},
    {EXT_X_SESSION_KEY,           sizeof(EXT_X_SESSION_KEY)-1,           handle_session_key},
    {EXT_X_CONTENT_STEERING,      sizeof(EXT_X_CONTENT_STEERING)-1,      handle_content_steering},
    /* Playlist metadata tags */
    {EXT_X_TARGETDURATION,        sizeof(EXT_X_TARGETDURATION)-1,        handle_targetduration},
    {EXT_X_MEDIA_SEQUENCE,        sizeof(EXT_X_MEDIA_SEQUENCE)-1,        handle_media_sequence},
    {EXT_X_DISCONTINUITY_SEQUENCE,sizeof(EXT_X_DISCONTINUITY_SEQUENCE)-1,handle_discontinuity_sequence},
    {EXT_X_PLAYLIST_TYPE,         sizeof(EXT_X_PLAYLIST_TYPE)-1,         handle_playlist_type},
    {EXT_X_VERSION,               sizeof(EXT_X_VERSION)-1,               handle_version},
    {EXT_X_ALLOW_CACHE,           sizeof(EXT_X_ALLOW_CACHE)-1,           handle_allow_cache},
    {EXT_X_ENDLIST,               sizeof(EXT_X_ENDLIST)-1,               handle_endlist},
    {EXT_I_FRAMES_ONLY,           sizeof(EXT_I_FRAMES_ONLY)-1,           handle_i_frames_only},
    {EXT_IS_INDEPENDENT_SEGMENTS, sizeof(EXT_IS_INDEPENDENT_SEGMENTS)-1, handle_independent_segments},
    {EXT_X_IMAGES_ONLY,           sizeof(EXT_X_IMAGES_ONLY)-1,           handle_images_only},
    /* Low-latency HLS tags */
    {EXT_X_SERVER_CONTROL,        sizeof(EXT_X_SERVER_CONTROL)-1,        handle_server_control},
    {EXT_X_PART_INF,              sizeof(EXT_X_PART_INF)-1,              handle_part_inf},
    {EXT_X_RENDITION_REPORT,      sizeof(EXT_X_RENDITION_REPORT)-1,      handle_rendition_report},
    {EXT_X_SKIP,                  sizeof(EXT_X_SKIP)-1,                  handle_skip},
    {EXT_X_PRELOAD_HINT,          sizeof(EXT_X_PRELOAD_HINT)-1,          handle_preload_hint},
    /* SCTE-35 / Ad insertion tags */
    {EXT_X_CUE_OUT_CONT,          sizeof(EXT_X_CUE_OUT_CONT)-1,          handle_cue_out_cont},
    {EXT_X_CUE_OUT,               sizeof(EXT_X_CUE_OUT)-1,               handle_cue_out},
    {EXT_X_CUE_IN,                sizeof(EXT_X_CUE_IN)-1,                handle_cue_in},
    {EXT_X_CUE_SPAN,              sizeof(EXT_X_CUE_SPAN)-1,              handle_cue_span},
    {EXT_OATCLS_SCTE35,           sizeof(EXT_OATCLS_SCTE35)-1,           handle_oatcls_scte35},
    {EXT_X_ASSET,                 sizeof(EXT_X_ASSET)-1,                 handle_asset},
    /* Miscellaneous tags */
    {EXT_X_START,                 sizeof(EXT_X_START)-1,                 handle_start},
    {EXT_X_TILES,                 sizeof(EXT_X_TILES)-1,                 handle_tiles},
    {EXT_X_BLACKOUT,              sizeof(EXT_X_BLACKOUT)-1,              handle_blackout},
    /* Sentinel */
    {NULL, 0, NULL}
};

/*
 * Dispatch a tag to its handler using the dispatch table.
 *
 * Returns:
 *   1 if handler was found and executed successfully
 *   0 if no handler found (unknown tag)
 *  -1 if handler found but returned error (exception set)
 */
static int
dispatch_tag(ParseContext *ctx, const char *line, size_t line_len)
{
    /* Fast rejection: all M3U8 tags start with '#' */
    if (line_len < 4 || line[0] != '#') {
        return 0;
    }

    for (const TagDispatch *d = TAG_DISPATCH; d->tag != NULL; d++) {
        /* Skip if line is shorter than tag */
        if (line_len < d->tag_len) continue;

        if (strncmp(line, d->tag, d->tag_len) == 0) {
            /* Verify tag boundary: must end with ':' or be complete line */
            char next = line[d->tag_len];
            if (next == ':' || next == '\0') {
                if (d->handler(ctx, line) < 0) {
                    return -1;
                }
                return 1;  /* Handled */
            }
        }
    }
    return 0;  /* Not found */
}

/*
 * Main parse function.
 *
 * Parse M3U8 playlist content and return a dictionary with all data found.
 *
 * Args:
 *     content: The M3U8 playlist content as a string.
 *     strict: If True, raise exceptions for syntax errors (default: False).
 *     custom_tags_parser: Optional callable for parsing custom tags.
 *
 * Returns:
 *     A dictionary containing the parsed playlist data.
 */
static PyObject *
m3u8_parse(PyObject *module, PyObject *args, PyObject *kwargs)
{
    const char *content;
    Py_ssize_t content_len;  /* Get size directly - enables zero-copy parsing */
    int strict = 0;
    PyObject *custom_tags_parser = Py_None;

    static char *kwlist[] = {"content", "strict", "custom_tags_parser", NULL};

    /* Use s# to get pointer AND size directly from Python string object */
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#|pO", kwlist,
                                     &content, &content_len, &strict, &custom_tags_parser)) {
        return NULL;
    }

    /*
     * Match parser.py's behavior: lines = content.strip().splitlines()
     *
     * The Python parser strips leading/trailing whitespace *before* splitting,
     * which affects strict-mode error line numbers when the input has leading
     * newlines (common with triple-quoted test fixtures).
     */
    const char *trimmed = content;
    const char *trimmed_end = content + content_len;
    while (trimmed < trimmed_end && ascii_isspace((unsigned char)*trimmed)) {
        trimmed++;
    }
    while (trimmed_end > trimmed && ascii_isspace((unsigned char)*(trimmed_end - 1))) {
        trimmed_end--;
    }
    Py_ssize_t trimmed_len = (Py_ssize_t)(trimmed_end - trimmed);

    /* Get module state for cached objects */
    m3u8_state *mod_state = get_m3u8_state(module);

    /* Check strict mode validation */
    if (strict) {
        /* Import and call version_matching.validate */
        PyObject *version_matching = PyImport_ImportModule("openm3u8.version_matching");
        if (version_matching == NULL) {
            return NULL;
        }
        PyObject *validate = PyObject_GetAttrString(version_matching, "validate");
        if (validate == NULL) {
            Py_DECREF(version_matching);
            return NULL;
        }
        /* Build list like parser.py: content.strip().splitlines() */
        PyObject *lines_list = build_stripped_splitlines(trimmed);
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

    /* Initialize result data dict using interned strings */
    PyObject *data = init_parse_data(mod_state);
    if (data == NULL) {
        return NULL;
    }

    /* Initialize parser state dict */
    PyObject *state = init_parse_state(mod_state);
    if (state == NULL) {
        Py_DECREF(data);
        return NULL;
    }

    /*
     * Set up parse context with shadow state.
     * Shadow state avoids dict lookups for hot flags in the main loop.
     */
    ParseContext ctx = {
        .mod_state = mod_state,
        .data = data,
        .state = state,
        .strict = strict,
        .lineno = 0,
        .expect_segment = 0,   /* Matches init_parse_state */
        .expect_playlist = 0,  /* Matches init_parse_state */
    };

    /*
     * Zero-copy line parsing: Walk the buffer with pointers.
     * We use a single reusable line buffer for the null-terminated stripped line.
     * This avoids copying the entire content upfront (the strtok_r approach).
     */
    const char *p = trimmed;
    const char *end = trimmed + trimmed_len;

    /* Reusable line buffer - starts small, grows as needed */
    size_t line_buf_size = 256;
    char *line_buf = PyMem_Malloc(line_buf_size);
    if (line_buf == NULL) {
        Py_DECREF(data);
        Py_DECREF(state);
        return PyErr_NoMemory();
    }

    while (p < end) {
        ctx.lineno++;

        /* Find end of line using memchr (often hardware-optimized) */
        const char *line_start = p;
        const char *eol = p;
        while (eol < end && *eol != '\n' && *eol != '\r') {
            eol++;
        }
        Py_ssize_t line_len = eol - line_start;

        /* Strip leading whitespace */
        while (line_len > 0 && ascii_isspace((unsigned char)*line_start)) {
            line_start++;
            line_len--;
        }
        /* Strip trailing whitespace */
        while (line_len > 0 && ascii_isspace((unsigned char)line_start[line_len - 1])) {
            line_len--;
        }

        /* Advance p past the newline(s) for next iteration */
        if (eol < end) {
            if (*eol == '\r' && (eol + 1) < end && *(eol + 1) == '\n') {
                p = eol + 2;  /* Skip \r\n */
            } else {
                p = eol + 1;  /* Skip \n or \r */
            }
        } else {
            p = end;
        }

        /* Skip empty lines */
        if (line_len == 0) {
            continue;
        }

        /* Grow line buffer if needed */
        if ((size_t)line_len + 1 > line_buf_size) {
            line_buf_size = (size_t)line_len + 1;
            char *new_buf = PyMem_Realloc(line_buf, line_buf_size);
            if (new_buf == NULL) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return PyErr_NoMemory();
            }
            line_buf = new_buf;
        }

        /* Copy stripped line to null-terminated buffer */
        memcpy(line_buf, line_start, line_len);
        line_buf[line_len] = '\0';
        char *stripped = line_buf;

        /* Call custom tags parser if provided */
        if (stripped[0] == '#' && custom_tags_parser != Py_None && PyCallable_Check(custom_tags_parser)) {
            /* Sync shadow state to dict before callback (so it sees current state) */
            if (sync_shadow_to_dict(&ctx) < 0) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            PyObject *py_line = PyUnicode_FromString(stripped);
            PyObject *py_lineno = PyLong_FromLong(ctx.lineno);
            if (py_line == NULL || py_lineno == NULL) {
                Py_XDECREF(py_line);
                Py_XDECREF(py_lineno);
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            PyObject *call_args = PyTuple_Pack(4, py_line, py_lineno, data, state);
            Py_DECREF(py_line);
            Py_DECREF(py_lineno);
            if (call_args == NULL) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            PyObject *result = PyObject_Call(custom_tags_parser, call_args, NULL);
            Py_DECREF(call_args);
            if (!result) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            /* Sync shadow state from dict (callback may have modified it) */
            sync_shadow_from_dict(&ctx);
            int truth = PyObject_IsTrue(result);
            Py_DECREF(result);
            if (truth < 0) {
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            if (truth) {
                /* p has already been advanced to the next line at the top of the loop */
                continue;
            }
        }

        if (stripped[0] == '#') {
            /*
             * Tag dispatch using data-driven table lookup.
             * This replaces ~400 lines of if/else strcmp chain with a clean loop.
             * See TAG_DISPATCH table for the tag-to-handler mappings.
             */

            /* Handle #EXTM3U - just ignore it */
            if (strncmp(stripped, EXT_M3U, sizeof(EXT_M3U)-1) == 0) {
                continue;
            }

            /* Dispatch to handler via table lookup */
            int dispatch_result = dispatch_tag(&ctx, stripped, line_len);
            if (dispatch_result < 0) {
                /* Handler returned error */
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
            if (dispatch_result == 0) {
                /* Unknown tag - error in strict mode */
                if (ctx.strict) {
                    raise_parse_error(mod_state, ctx.lineno, stripped);
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
            }
        } else {
            /* Non-comment line - segment or playlist URI */
            /* Use shadow state for hot path checks (no dict lookups) */
            if (ctx.expect_segment) {
                if (parse_ts_chunk(mod_state, stripped, data, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
                ctx.expect_segment = 0;  /* parse_ts_chunk clears this */
            } else if (ctx.expect_playlist) {
                if (parse_variant_playlist(mod_state, stripped, data, state) < 0) {
                    PyMem_Free(line_buf);
                    Py_DECREF(data);
                    Py_DECREF(state);
                    return NULL;
                }
                ctx.expect_playlist = 0;  /* parse_variant_playlist clears this */
            } else if (strict) {
                raise_parse_error(mod_state, ctx.lineno, stripped);
                PyMem_Free(line_buf);
                Py_DECREF(data);
                Py_DECREF(state);
                return NULL;
            }
        }
        /* Loop continues with pointer already advanced */
    }

    PyMem_Free(line_buf);

    /* Handle remaining partial segment - use interned strings */
    PyObject *segment = dict_get_interned(state, mod_state->str_segment);
    if (segment) {
        PyObject *segments = dict_get_interned(data, mod_state->str_segments);
        if (segments && PyList_Append(segments, segment) < 0) {
            Py_DECREF(state);
            Py_DECREF(data);
            return NULL;
        }
    }

    Py_DECREF(state);
    return data;
}

/* Module methods */
static PyMethodDef m3u8_parser_methods[] = {
    {"parse", (PyCFunction)m3u8_parse, METH_VARARGS | METH_KEYWORDS,
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

/*
 * Module traverse function for GC - uses X-macro expansion.
 */
static int
m3u8_parser_traverse(PyObject *module, visitproc visit, void *arg)
{
    m3u8_state *state = get_m3u8_state(module);
    Py_VISIT(state->ParseError);
    Py_VISIT(state->datetime_cls);
    Py_VISIT(state->timedelta_cls);
    Py_VISIT(state->fromisoformat_meth);
    #define VISIT_INTERNED(name, str) Py_VISIT(state->name);
    INTERNED_STRINGS(VISIT_INTERNED)
    #undef VISIT_INTERNED
    return 0;
}

/*
 * Module clear function for GC - uses X-macro expansion.
 */
static int
m3u8_parser_clear(PyObject *module)
{
    m3u8_state *state = get_m3u8_state(module);
    Py_CLEAR(state->ParseError);
    Py_CLEAR(state->datetime_cls);
    Py_CLEAR(state->timedelta_cls);
    Py_CLEAR(state->fromisoformat_meth);
    #define CLEAR_INTERNED(name, str) Py_CLEAR(state->name);
    INTERNED_STRINGS(CLEAR_INTERNED)
    #undef CLEAR_INTERNED
    return 0;
}

/*
 * Module deallocation function.
 */
static void
m3u8_parser_free(void *module)
{
    m3u8_parser_clear((PyObject *)module);
}

/* Module definition */
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

/*
 * Module initialization.
 *
 * Creates the module, initializes module state, and sets up cached objects.
 */
PyMODINIT_FUNC
PyInit__m3u8_parser(void)
{
    PyObject *m = PyModule_Create(&m3u8_parser_module);
    if (m == NULL) {
        return NULL;
    }

    m3u8_state *state = get_m3u8_state(m);

    /* Initialize module state to NULL for safe cleanup on error */
    state->ParseError = NULL;
    state->datetime_cls = NULL;
    state->timedelta_cls = NULL;
    state->fromisoformat_meth = NULL;
    #define NULL_INTERNED(name, str) state->name = NULL;
    INTERNED_STRINGS(NULL_INTERNED)
    #undef NULL_INTERNED

    /* Import ParseError from openm3u8.parser to use the same exception class */
    PyObject *parser_module = PyImport_ImportModule("openm3u8.parser");
    if (parser_module != NULL) {
        state->ParseError = PyObject_GetAttrString(parser_module, "ParseError");
        Py_DECREF(parser_module);
    }

    /* Fallback: create our own ParseError if import fails */
    if (state->ParseError == NULL) {
        PyErr_Clear();
        state->ParseError = PyErr_NewException(
            "openm3u8._m3u8_parser.ParseError", PyExc_Exception, NULL);
        if (state->ParseError == NULL) {
            goto error;
        }
    }

    /* Add ParseError to module (PyModule_AddObject steals a reference on success) */
    Py_INCREF(state->ParseError);
    if (PyModule_AddObject(m, "ParseError", state->ParseError) < 0) {
        Py_DECREF(state->ParseError);
        goto error;
    }

    /* Initialize datetime cache */
    if (init_datetime_cache(state) < 0) {
        goto error;
    }

    /* Initialize interned strings for common dict keys */
    if (init_interned_strings(state) < 0) {
        goto error;
    }

    return m;

error:
    Py_DECREF(m);
    return NULL;
}

