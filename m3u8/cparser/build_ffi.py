"""
CFFI builder script for the m3u8 C parser.

This script compiles the C parser and generates Python bindings using CFFI.
Run this script to build the extension:

    python -m m3u8.cparser.build_ffi

Or it will be automatically built by setup.py during installation.
"""

from pathlib import Path

from cffi import FFI

ffibuilder = FFI()

# Get the directory containing this file
HERE = Path(__file__).parent.resolve()

# Read the C source
c_source = (HERE / "m3u8_parser.c").read_text()

# C declarations for CFFI
# These are the struct definitions and function prototypes that Python needs to know about
ffibuilder.cdef(
    """
    /* Key-Value attribute */
    typedef struct M3U8Attribute {
        char *key;
        char *value;
        struct M3U8Attribute *next;
    } M3U8Attribute;

    /* Encryption key */
    typedef struct M3U8Key {
        char *method;
        char *uri;
        char *iv;
        char *keyformat;
        char *keyformatversions;
        struct M3U8Key *next;
    } M3U8Key;

    /* Initialization section */
    typedef struct M3U8Map {
        char *uri;
        char *byterange;
        struct M3U8Map *next;
    } M3U8Map;

    /* Date range */
    typedef struct M3U8DateRange {
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
        M3U8Attribute *x_attrs;
        struct M3U8DateRange *next;
    } M3U8DateRange;

    /* Partial segment */
    typedef struct M3U8Part {
        char *uri;
        double duration;
        char *byterange;
        char *independent;
        char *gap;
        M3U8DateRange *dateranges;
        int gap_tag;
        struct M3U8Part *next;
    } M3U8Part;

    /* Segment */
    typedef struct M3U8Segment {
        double duration;
        char *title;
        char *uri;
        char *byterange;
        int bitrate;
        
        int discontinuity;
        char *program_date_time;
        
        int cue_in;
        int cue_out;
        int cue_out_start;
        int cue_out_explicitly_duration;
        char *scte35;
        char *oatcls_scte35;
        char *scte35_duration;
        char *scte35_elapsedtime;
        M3U8Attribute *asset_metadata;
        
        M3U8Key *key;
        M3U8Map *init_section;
        
        M3U8DateRange *dateranges;
        
        int gap_tag;
        char *blackout;
        
        M3U8Part *parts;
        
        struct M3U8Segment *next;
    } M3U8Segment;

    /* Variant playlist */
    typedef struct M3U8Playlist {
        char *uri;
        
        int program_id;
        long long bandwidth;
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
        
        struct M3U8Playlist *next;
    } M3U8Playlist;

    /* I-Frame playlist */
    typedef struct M3U8IFramePlaylist {
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
        
        struct M3U8IFramePlaylist *next;
    } M3U8IFramePlaylist;

    /* Image playlist */
    typedef struct M3U8ImagePlaylist {
        char *uri;
        
        int program_id;
        long long bandwidth;
        long long average_bandwidth;
        char *resolution;
        char *codecs;
        char *pathway_id;
        char *stable_variant_id;
        
        struct M3U8ImagePlaylist *next;
    } M3U8ImagePlaylist;

    /* Media */
    typedef struct M3U8Media {
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
        
        struct M3U8Media *next;
    } M3U8Media;

    /* Rendition report */
    typedef struct M3U8RenditionReport {
        char *uri;
        long long last_msn;
        long long last_part;
        int has_last_msn;
        int has_last_part;
        
        struct M3U8RenditionReport *next;
    } M3U8RenditionReport;

    /* Session data */
    typedef struct M3U8SessionData {
        char *data_id;
        char *value;
        char *uri;
        char *language;
        
        struct M3U8SessionData *next;
    } M3U8SessionData;

    /* Tiles */
    typedef struct M3U8Tiles {
        char *resolution;
        char *layout;
        double duration;
        char *uri;
        
        struct M3U8Tiles *next;
    } M3U8Tiles;

    /* Root data structure */
    typedef struct M3U8Data {
        int targetduration;
        long long media_sequence;
        long long discontinuity_sequence;
        int version;
        char *allow_cache;
        char *playlist_type;
        char *program_date_time;
        
        int is_variant;
        int is_endlist;
        int is_i_frames_only;
        int is_independent_segments;
        int is_images_only;
        int has_media_sequence;
        
        double start_time_offset;
        int has_start;
        char *start_precise;
        
        char *server_control_can_block_reload;
        double server_control_hold_back;
        double server_control_part_hold_back;
        double server_control_can_skip_until;
        char *server_control_can_skip_dateranges;
        int has_server_control;
        
        double part_inf_part_target;
        int has_part_inf;
        
        int skip_skipped_segments;
        char *skip_recently_removed_dateranges;
        int has_skip;
        
        char *preload_hint_type;
        char *preload_hint_uri;
        int preload_hint_byterange_start;
        int preload_hint_byterange_length;
        int has_preload_hint;
        int has_preload_hint_byterange_start;
        int has_preload_hint_byterange_length;
        
        char *content_steering_server_uri;
        char *content_steering_pathway_id;
        int has_content_steering;
        
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
    } M3U8Data;

    /* Public API */
    M3U8Data* m3u8_parse(const char *content, size_t length);
    void m3u8_free(M3U8Data *data);
    """
)

# Tell CFFI to compile our C source
ffibuilder.set_source(
    "m3u8._m3u8_cparser",  # Module name
    c_source,  # Include the full C source
    include_dirs=[str(HERE)],
)

if __name__ == "__main__":
    ffibuilder.compile(verbose=True)
