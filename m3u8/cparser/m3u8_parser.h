/*
 * m3u8_parser.h - Pure C parser for HLS M3U8 manifests
 *
 * This header defines the data structures for parsing M3U8 playlists.
 * The parser knows nothing about Python - it operates purely on C strings
 * and returns C structs that are later converted to Python objects.
 */

#ifndef M3U8_PARSER_H
#define M3U8_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Forward declarations
 * ========================================================================= */

typedef struct M3U8Attribute M3U8Attribute;
typedef struct M3U8Key M3U8Key;
typedef struct M3U8Map M3U8Map;
typedef struct M3U8DateRange M3U8DateRange;
typedef struct M3U8Part M3U8Part;
typedef struct M3U8Segment M3U8Segment;
typedef struct M3U8Playlist M3U8Playlist;
typedef struct M3U8IFramePlaylist M3U8IFramePlaylist;
typedef struct M3U8ImagePlaylist M3U8ImagePlaylist;
typedef struct M3U8Media M3U8Media;
typedef struct M3U8RenditionReport M3U8RenditionReport;
typedef struct M3U8SessionData M3U8SessionData;
typedef struct M3U8Tiles M3U8Tiles;
typedef struct M3U8Data M3U8Data;

/* ============================================================================
 * Key-Value attribute (used for generic attribute storage)
 * ========================================================================= */

struct M3U8Attribute {
    char *key;
    char *value;
    M3U8Attribute *next;
};

/* ============================================================================
 * Encryption key (#EXT-X-KEY or #EXT-X-SESSION-KEY)
 * ========================================================================= */

struct M3U8Key {
    char *method;
    char *uri;
    char *iv;
    char *keyformat;
    char *keyformatversions;
    M3U8Key *next;
};

/* ============================================================================
 * Initialization section (#EXT-X-MAP)
 * ========================================================================= */

struct M3U8Map {
    char *uri;
    char *byterange;
    M3U8Map *next;
};

/* ============================================================================
 * Date range (#EXT-X-DATERANGE)
 * ========================================================================= */

struct M3U8DateRange {
    char *id;
    char *class_name;
    char *start_date;
    char *end_date;
    double duration;
    double planned_duration;
    char *scte35_cmd;
    char *scte35_out;
    char *scte35_in;
    char *end_on_next;
    M3U8Attribute *x_attrs;  /* Custom X-* attributes */
    M3U8DateRange *next;
};

/* ============================================================================
 * Partial segment (#EXT-X-PART)
 * ========================================================================= */

struct M3U8Part {
    char *uri;
    double duration;
    char *byterange;
    char *independent;
    char *gap;
    M3U8DateRange *dateranges;
    int gap_tag;
    M3U8Part *next;
};

/* ============================================================================
 * Segment (#EXTINF + URI line)
 * ========================================================================= */

struct M3U8Segment {
    double duration;
    char *title;
    char *uri;
    char *byterange;
    int bitrate;
    
    /* Discontinuity and program date time */
    int discontinuity;
    char *program_date_time;
    
    /* Cue markers */
    int cue_in;
    int cue_out;
    int cue_out_start;
    int cue_out_explicitly_duration;
    char *scte35;
    char *oatcls_scte35;
    char *scte35_duration;
    char *scte35_elapsedtime;
    M3U8Attribute *asset_metadata;
    
    /* Associated key and map */
    M3U8Key *key;
    M3U8Map *init_section;
    
    /* Date ranges */
    M3U8DateRange *dateranges;
    
    /* Gap and blackout */
    int gap_tag;
    char *blackout;
    
    /* Partial segments */
    M3U8Part *parts;
    
    M3U8Segment *next;
};

/* ============================================================================
 * Variant playlist (#EXT-X-STREAM-INF + URI)
 * ========================================================================= */

struct M3U8Playlist {
    char *uri;
    
    /* Stream info attributes */
    int program_id;
    long long bandwidth;            /* Can be very large for high-bitrate streams */
    long long average_bandwidth;
    char *resolution;
    char *codecs;
    double frame_rate;
    char *video;
    char *audio;
    char *subtitles;
    char *closed_captions;
    char *video_range;
    char *hdcp_level;
    char *pathway_id;
    char *stable_variant_id;
    char *req_video_layout;
    
    M3U8Playlist *next;
};

/* ============================================================================
 * I-Frame playlist (#EXT-X-I-FRAME-STREAM-INF)
 * ========================================================================= */

struct M3U8IFramePlaylist {
    char *uri;
    
    int program_id;
    long long bandwidth;
    long long average_bandwidth;
    char *resolution;
    char *codecs;
    char *video_range;
    char *hdcp_level;
    char *pathway_id;
    char *stable_variant_id;
    
    M3U8IFramePlaylist *next;
};

/* ============================================================================
 * Image playlist (#EXT-X-IMAGE-STREAM-INF)
 * ========================================================================= */

struct M3U8ImagePlaylist {
    char *uri;
    
    int program_id;
    long long bandwidth;
    long long average_bandwidth;
    char *resolution;
    char *codecs;
    char *pathway_id;
    char *stable_variant_id;
    
    M3U8ImagePlaylist *next;
};

/* ============================================================================
 * Media (#EXT-X-MEDIA)
 * ========================================================================= */

struct M3U8Media {
    char *type;
    char *uri;
    char *group_id;
    char *language;
    char *assoc_language;
    char *name;
    char *default_val;
    char *autoselect;
    char *forced;
    char *instream_id;
    char *characteristics;
    char *channels;
    char *stable_rendition_id;
    
    M3U8Media *next;
};

/* ============================================================================
 * Rendition report (#EXT-X-RENDITION-REPORT)
 * ========================================================================= */

struct M3U8RenditionReport {
    char *uri;
    long long last_msn;             /* Media sequence number can be large */
    long long last_part;
    int has_last_msn;
    int has_last_part;
    
    M3U8RenditionReport *next;
};

/* ============================================================================
 * Session data (#EXT-X-SESSION-DATA)
 * ========================================================================= */

struct M3U8SessionData {
    char *data_id;
    char *value;
    char *uri;
    char *language;
    
    M3U8SessionData *next;
};

/* ============================================================================
 * Tiles (#EXT-X-TILES)
 * ========================================================================= */

struct M3U8Tiles {
    char *resolution;
    char *layout;
    double duration;
    char *uri;
    
    M3U8Tiles *next;
};

/* ============================================================================
 * Root data structure
 * ========================================================================= */

struct M3U8Data {
    /* Top-level attributes */
    int targetduration;
    long long media_sequence;       /* Can get very large for long-running streams */
    long long discontinuity_sequence; /* Same reason */
    int version;
    char *allow_cache;
    char *playlist_type;
    char *program_date_time;
    
    /* Flags */
    int is_variant;
    int is_endlist;
    int is_i_frames_only;
    int is_independent_segments;
    int is_images_only;
    int has_media_sequence;
    
    /* Start */
    double start_time_offset;
    int has_start;
    char *start_precise;
    
    /* Server control */
    char *server_control_can_block_reload;
    double server_control_hold_back;
    double server_control_part_hold_back;
    double server_control_can_skip_until;
    char *server_control_can_skip_dateranges;
    int has_server_control;
    
    /* Part info */
    double part_inf_part_target;
    int has_part_inf;
    
    /* Skip */
    int skip_skipped_segments;
    char *skip_recently_removed_dateranges;
    int has_skip;
    
    /* Preload hint */
    char *preload_hint_type;
    char *preload_hint_uri;
    int preload_hint_byterange_start;
    int preload_hint_byterange_length;
    int has_preload_hint;
    int has_preload_hint_byterange_start;
    int has_preload_hint_byterange_length;
    
    /* Content steering */
    char *content_steering_server_uri;
    char *content_steering_pathway_id;
    int has_content_steering;
    
    /* Collections (linked lists) */
    M3U8Segment *segments_head;
    M3U8Segment *segments_tail;
    
    M3U8Playlist *playlists_head;
    M3U8Playlist *playlists_tail;
    
    M3U8IFramePlaylist *iframe_playlists_head;
    M3U8IFramePlaylist *iframe_playlists_tail;
    
    M3U8ImagePlaylist *image_playlists_head;
    M3U8ImagePlaylist *image_playlists_tail;
    
    M3U8Media *media_head;
    M3U8Media *media_tail;
    
    M3U8Key *keys_head;
    M3U8Key *keys_tail;
    
    M3U8Key *session_keys_head;
    M3U8Key *session_keys_tail;
    
    M3U8Map *segment_map_head;
    M3U8Map *segment_map_tail;
    
    M3U8RenditionReport *rendition_reports_head;
    M3U8RenditionReport *rendition_reports_tail;
    
    M3U8SessionData *session_data_head;
    M3U8SessionData *session_data_tail;
    
    M3U8Tiles *tiles_head;
    M3U8Tiles *tiles_tail;
};

/* ============================================================================
 * Public API
 * ========================================================================= */

/**
 * Parse an M3U8 playlist from a string buffer.
 *
 * @param content  The M3U8 content as a null-terminated string
 * @param length   The length of the content (excluding null terminator)
 * @return         A pointer to the parsed data structure, or NULL on error
 */
M3U8Data* m3u8_parse(const char *content, size_t length);

/**
 * Free all memory associated with a parsed M3U8 data structure.
 *
 * @param data  The data structure to free
 */
void m3u8_free(M3U8Data *data);

#ifdef __cplusplus
}
#endif

#endif /* M3U8_PARSER_H */
