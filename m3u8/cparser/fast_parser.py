"""
Python wrapper for the CFFI-based C parser.

This module converts the C structs returned by the C parser into Python
dictionaries that match the format expected by the existing m3u8 library.
"""

from __future__ import annotations


def _decode(cdata) -> str | None:
    """Decode a C string to Python string, or return None if NULL."""
    if cdata == _ffi.NULL:
        return None
    return _ffi.string(cdata).decode("utf-8")


def _convert_attributes(c_attrs) -> dict:
    """Convert a linked list of M3U8Attribute to a Python dict."""
    result = {}
    current = c_attrs
    while current != _ffi.NULL:
        key = _decode(current.key)
        value = _decode(current.value)
        if key is not None:
            result[key] = value
        current = current.next
    return result


def _convert_daterange(c_dr) -> dict:
    """Convert an M3U8DateRange to a Python dict."""
    result = {"id": _decode(c_dr.id)}

    class_name = _decode(c_dr.class_name)
    if class_name is not None:
        result["class"] = class_name

    start_date = _decode(c_dr.start_date)
    if start_date is not None:
        result["start_date"] = start_date

    end_date = _decode(c_dr.end_date)
    if end_date is not None:
        result["end_date"] = end_date

    if c_dr.duration != 0:
        result["duration"] = c_dr.duration

    if c_dr.planned_duration != 0:
        result["planned_duration"] = c_dr.planned_duration

    scte35_cmd = _decode(c_dr.scte35_cmd)
    if scte35_cmd is not None:
        result["scte35_cmd"] = scte35_cmd

    scte35_out = _decode(c_dr.scte35_out)
    if scte35_out is not None:
        result["scte35_out"] = scte35_out

    scte35_in = _decode(c_dr.scte35_in)
    if scte35_in is not None:
        result["scte35_in"] = scte35_in

    end_on_next = _decode(c_dr.end_on_next)
    if end_on_next is not None:
        result["end_on_next"] = end_on_next

    # Add X-* attributes
    x_attrs = _convert_attributes(c_dr.x_attrs)
    result.update(x_attrs)

    return result


def _convert_dateranges(c_dr) -> list | None:
    """Convert a linked list of M3U8DateRange to a Python list."""
    if c_dr == _ffi.NULL:
        return None

    result = []
    current = c_dr
    while current != _ffi.NULL:
        result.append(_convert_daterange(current))
        current = current.next

    # Reverse to get original order (linked list was built in reverse)
    return list(reversed(result)) if result else None


def _convert_part(c_part) -> dict:
    """Convert an M3U8Part to a Python dict."""
    result = {
        "uri": _decode(c_part.uri),
        "duration": c_part.duration,
    }

    byterange = _decode(c_part.byterange)
    if byterange is not None:
        result["byterange"] = byterange

    independent = _decode(c_part.independent)
    if independent is not None:
        result["independent"] = independent

    gap = _decode(c_part.gap)
    if gap is not None:
        result["gap"] = gap

    dateranges = _convert_dateranges(c_part.dateranges)
    result["dateranges"] = dateranges

    result["gap_tag"] = True if c_part.gap_tag else None

    return result


def _convert_parts(c_part) -> list | None:
    """Convert a linked list of M3U8Part to a Python list."""
    if c_part == _ffi.NULL:
        return None

    result = []
    current = c_part
    while current != _ffi.NULL:
        result.append(_convert_part(current))
        current = current.next

    return result if result else None


def _convert_key(c_key) -> dict | None:
    """Convert an M3U8Key to a Python dict."""
    if c_key == _ffi.NULL:
        return None

    result = {}

    method = _decode(c_key.method)
    if method is not None:
        result["method"] = method

    uri = _decode(c_key.uri)
    if uri is not None:
        result["uri"] = uri

    iv = _decode(c_key.iv)
    if iv is not None:
        result["iv"] = iv

    keyformat = _decode(c_key.keyformat)
    if keyformat is not None:
        result["keyformat"] = keyformat

    keyformatversions = _decode(c_key.keyformatversions)
    if keyformatversions is not None:
        result["keyformatversions"] = keyformatversions

    return result


def _convert_keys(c_key) -> list:
    """Convert a linked list of M3U8Key to a Python list."""
    result = []
    current = c_key
    while current != _ffi.NULL:
        result.append(_convert_key(current))
        current = current.next
    return result


def _convert_map(c_map) -> dict | None:
    """Convert an M3U8Map to a Python dict."""
    if c_map == _ffi.NULL:
        return None

    result = {"uri": _decode(c_map.uri)}

    byterange = _decode(c_map.byterange)
    if byterange is not None:
        result["byterange"] = byterange

    return result


def _convert_maps(c_map) -> list:
    """Convert a linked list of M3U8Map to a Python list."""
    result = []
    current = c_map
    while current != _ffi.NULL:
        result.append(_convert_map(current))
        current = current.next
    return result


def _convert_segment(c_seg) -> dict:
    """Convert an M3U8Segment to a Python dict."""
    result = {
        "duration": c_seg.duration,
        "title": _decode(c_seg.title) or "",
        "uri": _decode(c_seg.uri),
        "discontinuity": bool(c_seg.discontinuity),
        "cue_in": bool(c_seg.cue_in),
        "cue_out": bool(c_seg.cue_out),
        "cue_out_start": bool(c_seg.cue_out_start),
        "cue_out_explicitly_duration": bool(c_seg.cue_out_explicitly_duration),
        "scte35": _decode(c_seg.scte35),
        "oatcls_scte35": _decode(c_seg.oatcls_scte35),
        "scte35_duration": _decode(c_seg.scte35_duration),
        "scte35_elapsedtime": _decode(c_seg.scte35_elapsedtime),
        "dateranges": _convert_dateranges(c_seg.dateranges),
        "gap_tag": True if c_seg.gap_tag else None,
    }

    byterange = _decode(c_seg.byterange)
    if byterange is not None:
        result["byterange"] = byterange

    if c_seg.bitrate != 0:
        result["bitrate"] = c_seg.bitrate

    program_date_time = _decode(c_seg.program_date_time)
    if program_date_time is not None:
        from m3u8.parser import cast_date_time

        result["program_date_time"] = cast_date_time(program_date_time)

    # Key - only include if segment has a key (matches Python parser behavior)
    if c_seg.key != _ffi.NULL:
        result["key"] = _convert_key(c_seg.key)

    # Init section
    if c_seg.init_section != _ffi.NULL:
        result["init_section"] = _convert_map(c_seg.init_section)

    # Parts
    parts = _convert_parts(c_seg.parts)
    if parts:
        result["parts"] = parts

    # Asset metadata
    if c_seg.asset_metadata != _ffi.NULL:
        result["asset_metadata"] = _convert_attributes(c_seg.asset_metadata)
    else:
        result["asset_metadata"] = None

    # Blackout
    blackout = _decode(c_seg.blackout)
    if blackout == "__BLACKOUT_TRUE__":
        # Special marker for bare #EXT-X-BLACKOUT tag without parameters
        result["blackout"] = True
    elif blackout is not None:
        result["blackout"] = blackout
    else:
        result["blackout"] = None

    return result


def _convert_segments(c_seg) -> list:
    """Convert a linked list of M3U8Segment to a Python list."""
    from datetime import timedelta

    from m3u8.parser import cast_date_time

    result = []
    current_program_date_time = None
    current = c_seg
    while current != _ffi.NULL:
        seg = _convert_segment(current)

        # Track and propagate current_program_date_time
        # If this segment has a program_date_time, use it as the new current
        if "program_date_time" in seg and seg["program_date_time"] is not None:
            current_program_date_time = seg["program_date_time"]

        # Add current_program_date_time to segment if we have one
        if current_program_date_time is not None:
            seg["current_program_date_time"] = current_program_date_time
            # Increment for next segment
            current_program_date_time = current_program_date_time + timedelta(
                seconds=seg["duration"]
            )

        result.append(seg)
        current = current.next
    return result


def _convert_playlist(c_pl) -> dict:
    """Convert an M3U8Playlist to a Python dict."""
    stream_info = {
        "bandwidth": c_pl.bandwidth,
    }

    if c_pl.program_id != 0:
        stream_info["program_id"] = c_pl.program_id

    if c_pl.average_bandwidth != 0:
        stream_info["average_bandwidth"] = c_pl.average_bandwidth

    resolution = _decode(c_pl.resolution)
    if resolution is not None:
        stream_info["resolution"] = resolution

    codecs = _decode(c_pl.codecs)
    if codecs is not None:
        stream_info["codecs"] = codecs

    if c_pl.frame_rate != 0:
        stream_info["frame_rate"] = c_pl.frame_rate

    video = _decode(c_pl.video)
    if video is not None:
        stream_info["video"] = video

    audio = _decode(c_pl.audio)
    if audio is not None:
        stream_info["audio"] = audio

    subtitles = _decode(c_pl.subtitles)
    if subtitles is not None:
        stream_info["subtitles"] = subtitles

    closed_captions = _decode(c_pl.closed_captions)
    if closed_captions is not None:
        stream_info["closed_captions"] = closed_captions

    video_range = _decode(c_pl.video_range)
    if video_range is not None:
        stream_info["video_range"] = video_range

    hdcp_level = _decode(c_pl.hdcp_level)
    if hdcp_level is not None:
        stream_info["hdcp_level"] = hdcp_level

    pathway_id = _decode(c_pl.pathway_id)
    if pathway_id is not None:
        stream_info["pathway_id"] = pathway_id

    stable_variant_id = _decode(c_pl.stable_variant_id)
    if stable_variant_id is not None:
        stream_info["stable_variant_id"] = stable_variant_id

    req_video_layout = _decode(c_pl.req_video_layout)
    if req_video_layout is not None:
        stream_info["req_video_layout"] = req_video_layout

    return {
        "uri": _decode(c_pl.uri),
        "stream_info": stream_info,
    }


def _convert_playlists(c_pl) -> list:
    """Convert a linked list of M3U8Playlist to a Python list."""
    result = []
    current = c_pl
    while current != _ffi.NULL:
        result.append(_convert_playlist(current))
        current = current.next
    return result


def _convert_iframe_playlist(c_pl) -> dict:
    """Convert an M3U8IFramePlaylist to a Python dict."""
    iframe_stream_info = {}

    if c_pl.program_id != 0:
        iframe_stream_info["program_id"] = c_pl.program_id

    if c_pl.bandwidth != 0:
        iframe_stream_info["bandwidth"] = c_pl.bandwidth

    if c_pl.average_bandwidth != 0:
        iframe_stream_info["average_bandwidth"] = c_pl.average_bandwidth

    resolution = _decode(c_pl.resolution)
    if resolution is not None:
        iframe_stream_info["resolution"] = resolution

    codecs = _decode(c_pl.codecs)
    if codecs is not None:
        iframe_stream_info["codecs"] = codecs

    video_range = _decode(c_pl.video_range)
    if video_range is not None:
        iframe_stream_info["video_range"] = video_range

    hdcp_level = _decode(c_pl.hdcp_level)
    if hdcp_level is not None:
        iframe_stream_info["hdcp_level"] = hdcp_level

    pathway_id = _decode(c_pl.pathway_id)
    if pathway_id is not None:
        iframe_stream_info["pathway_id"] = pathway_id

    stable_variant_id = _decode(c_pl.stable_variant_id)
    if stable_variant_id is not None:
        iframe_stream_info["stable_variant_id"] = stable_variant_id

    return {
        "uri": _decode(c_pl.uri),
        "iframe_stream_info": iframe_stream_info,
    }


def _convert_iframe_playlists(c_pl) -> list:
    """Convert a linked list of M3U8IFramePlaylist to a Python list."""
    result = []
    current = c_pl
    while current != _ffi.NULL:
        result.append(_convert_iframe_playlist(current))
        current = current.next
    return result


def _convert_image_playlist(c_pl) -> dict:
    """Convert an M3U8ImagePlaylist to a Python dict."""
    image_stream_info = {}

    if c_pl.program_id != 0:
        image_stream_info["program_id"] = c_pl.program_id

    if c_pl.bandwidth != 0:
        image_stream_info["bandwidth"] = c_pl.bandwidth

    if c_pl.average_bandwidth != 0:
        image_stream_info["average_bandwidth"] = c_pl.average_bandwidth

    resolution = _decode(c_pl.resolution)
    if resolution is not None:
        image_stream_info["resolution"] = resolution

    codecs = _decode(c_pl.codecs)
    if codecs is not None:
        image_stream_info["codecs"] = codecs

    pathway_id = _decode(c_pl.pathway_id)
    if pathway_id is not None:
        image_stream_info["pathway_id"] = pathway_id

    stable_variant_id = _decode(c_pl.stable_variant_id)
    if stable_variant_id is not None:
        image_stream_info["stable_variant_id"] = stable_variant_id

    return {
        "uri": _decode(c_pl.uri),
        "image_stream_info": image_stream_info,
    }


def _convert_image_playlists(c_pl) -> list:
    """Convert a linked list of M3U8ImagePlaylist to a Python list."""
    result = []
    current = c_pl
    while current != _ffi.NULL:
        result.append(_convert_image_playlist(current))
        current = current.next
    return result


def _convert_media(c_media) -> dict:
    """Convert an M3U8Media to a Python dict."""
    result = {}

    media_type = _decode(c_media.type)
    if media_type is not None:
        result["type"] = media_type

    uri = _decode(c_media.uri)
    if uri is not None:
        result["uri"] = uri

    group_id = _decode(c_media.group_id)
    if group_id is not None:
        result["group_id"] = group_id

    language = _decode(c_media.language)
    if language is not None:
        result["language"] = language

    assoc_language = _decode(c_media.assoc_language)
    if assoc_language is not None:
        result["assoc_language"] = assoc_language

    name = _decode(c_media.name)
    if name is not None:
        result["name"] = name

    default_val = _decode(c_media.default_val)
    if default_val is not None:
        result["default"] = default_val

    autoselect = _decode(c_media.autoselect)
    if autoselect is not None:
        result["autoselect"] = autoselect

    forced = _decode(c_media.forced)
    if forced is not None:
        result["forced"] = forced

    instream_id = _decode(c_media.instream_id)
    if instream_id is not None:
        result["instream_id"] = instream_id

    characteristics = _decode(c_media.characteristics)
    if characteristics is not None:
        result["characteristics"] = characteristics

    channels = _decode(c_media.channels)
    if channels is not None:
        result["channels"] = channels

    stable_rendition_id = _decode(c_media.stable_rendition_id)
    if stable_rendition_id is not None:
        result["stable_rendition_id"] = stable_rendition_id

    return result


def _convert_media_list(c_media) -> list:
    """Convert a linked list of M3U8Media to a Python list."""
    result = []
    current = c_media
    while current != _ffi.NULL:
        result.append(_convert_media(current))
        current = current.next
    return result


def _convert_rendition_report(c_rr) -> dict:
    """Convert an M3U8RenditionReport to a Python dict."""
    result = {"uri": _decode(c_rr.uri)}

    if c_rr.has_last_msn:
        result["last_msn"] = c_rr.last_msn

    if c_rr.has_last_part:
        result["last_part"] = c_rr.last_part

    return result


def _convert_rendition_reports(c_rr) -> list:
    """Convert a linked list of M3U8RenditionReport to a Python list."""
    result = []
    current = c_rr
    while current != _ffi.NULL:
        result.append(_convert_rendition_report(current))
        current = current.next
    return result


def _convert_session_data(c_sd) -> dict:
    """Convert an M3U8SessionData to a Python dict."""
    result = {}

    data_id = _decode(c_sd.data_id)
    if data_id is not None:
        result["data_id"] = data_id

    value = _decode(c_sd.value)
    if value is not None:
        result["value"] = value

    uri = _decode(c_sd.uri)
    if uri is not None:
        result["uri"] = uri

    language = _decode(c_sd.language)
    if language is not None:
        result["language"] = language

    return result


def _convert_session_data_list(c_sd) -> list:
    """Convert a linked list of M3U8SessionData to a Python list."""
    result = []
    current = c_sd
    while current != _ffi.NULL:
        result.append(_convert_session_data(current))
        current = current.next
    return result


def _convert_tiles(c_tiles) -> dict:
    """Convert an M3U8Tiles to a Python dict."""
    result = {}

    resolution = _decode(c_tiles.resolution)
    if resolution is not None:
        result["resolution"] = resolution

    layout = _decode(c_tiles.layout)
    if layout is not None:
        result["layout"] = layout

    if c_tiles.duration != 0:
        result["duration"] = c_tiles.duration

    uri = _decode(c_tiles.uri)
    if uri is not None:
        result["uri"] = uri

    return result


def _convert_tiles_list(c_tiles) -> list:
    """Convert a linked list of M3U8Tiles to a Python list."""
    result = []
    current = c_tiles
    while current != _ffi.NULL:
        result.append(_convert_tiles(current))
        current = current.next
    return result


def parse(content: str, strict: bool = False, custom_tags_parser=None) -> dict:
    """
    Parse an M3U8 playlist using the fast C parser.

    Args:
        content: The M3U8 playlist content as a string.
        strict: If True, raise an exception on parse errors.
                Note: The C parser does not support strict mode - it will
                fall back to the Python parser if strict=True.
        custom_tags_parser: A callable for parsing custom tags.
                Note: The C parser does not support custom tag parsers -
                it will fall back to the Python parser if provided.

    Returns:
        A dictionary containing the parsed playlist data.

    Raises:
        ImportError: If the C extension is not available.
    """
    # Fall back to Python parser for features not supported by C parser
    if strict or custom_tags_parser is not None:
        from m3u8.parser import parse as py_parse

        return py_parse(content, strict=strict, custom_tags_parser=custom_tags_parser)

    # Encode content to bytes
    b_content = content.encode("utf-8")

    # Call C parser
    c_data = _lib.m3u8_parse(b_content, len(b_content))

    if c_data == _ffi.NULL:
        # Return empty data structure on error
        return {
            "media_sequence": 0,
            "is_variant": False,
            "is_endlist": False,
            "is_i_frames_only": False,
            "is_independent_segments": False,
            "is_images_only": False,
            "playlist_type": None,
            "playlists": [],
            "segments": [],
            "iframe_playlists": [],
            "image_playlists": [],
            "tiles": [],
            "media": [],
            "keys": [],
            "rendition_reports": [],
            "skip": {},
            "part_inf": {},
            "session_data": [],
            "session_keys": [],
            "segment_map": [],
        }

    try:
        # Convert C structs to Python dicts
        data = {
            "media_sequence": c_data.media_sequence if c_data.has_media_sequence else (
                None if c_data.is_variant else 0
            ),
            "is_variant": bool(c_data.is_variant),
            "is_endlist": bool(c_data.is_endlist),
            "is_i_frames_only": bool(c_data.is_i_frames_only),
            "is_independent_segments": bool(c_data.is_independent_segments),
            "is_images_only": bool(c_data.is_images_only),
            "playlist_type": _decode(c_data.playlist_type),
            "playlists": _convert_playlists(c_data.playlists_head),
            "segments": _convert_segments(c_data.segments_head),
            "iframe_playlists": _convert_iframe_playlists(c_data.iframe_playlists_head),
            "image_playlists": _convert_image_playlists(c_data.image_playlists_head),
            "tiles": _convert_tiles_list(c_data.tiles_head),
            "media": _convert_media_list(c_data.media_head),
            "keys": _convert_keys(c_data.keys_head),
            "rendition_reports": _convert_rendition_reports(
                c_data.rendition_reports_head
            ),
            "session_data": _convert_session_data_list(c_data.session_data_head),
            "session_keys": _convert_keys(c_data.session_keys_head),
            "segment_map": _convert_maps(c_data.segment_map_head),
        }

        # Add optional top-level fields
        if c_data.targetduration != 0:
            data["targetduration"] = c_data.targetduration

        if c_data.discontinuity_sequence != 0:
            data["discontinuity_sequence"] = c_data.discontinuity_sequence

        if c_data.version != 0:
            data["version"] = c_data.version

        allow_cache = _decode(c_data.allow_cache)
        if allow_cache is not None:
            data["allow_cache"] = allow_cache

        program_date_time = _decode(c_data.program_date_time)
        if program_date_time is not None:
            # Convert string to datetime for compatibility
            from m3u8.parser import cast_date_time

            data["program_date_time"] = cast_date_time(program_date_time)

        # Start
        if c_data.has_start:
            data["start"] = {"time_offset": c_data.start_time_offset}
            start_precise = _decode(c_data.start_precise)
            if start_precise is not None:
                data["start"]["precise"] = start_precise

        # Server control
        if c_data.has_server_control:
            server_control = {}
            can_block_reload = _decode(c_data.server_control_can_block_reload)
            if can_block_reload is not None:
                server_control["can_block_reload"] = can_block_reload
            if c_data.server_control_hold_back != 0:
                server_control["hold_back"] = c_data.server_control_hold_back
            if c_data.server_control_part_hold_back != 0:
                server_control["part_hold_back"] = c_data.server_control_part_hold_back
            if c_data.server_control_can_skip_until != 0:
                server_control["can_skip_until"] = c_data.server_control_can_skip_until
            can_skip_dateranges = _decode(c_data.server_control_can_skip_dateranges)
            if can_skip_dateranges is not None:
                server_control["can_skip_dateranges"] = can_skip_dateranges
            data["server_control"] = server_control

        # Part inf
        if c_data.has_part_inf:
            data["part_inf"] = {"part_target": c_data.part_inf_part_target}
        else:
            data["part_inf"] = {}

        # Skip
        if c_data.has_skip:
            skip = {"skipped_segments": c_data.skip_skipped_segments}
            recently_removed = _decode(c_data.skip_recently_removed_dateranges)
            if recently_removed is not None:
                skip["recently_removed_dateranges"] = recently_removed
            data["skip"] = skip
        else:
            data["skip"] = {}

        # Preload hint
        if c_data.has_preload_hint:
            preload_hint = {
                "type": _decode(c_data.preload_hint_type),
                "uri": _decode(c_data.preload_hint_uri),
            }
            if c_data.has_preload_hint_byterange_start:
                preload_hint["byterange_start"] = c_data.preload_hint_byterange_start
            if c_data.has_preload_hint_byterange_length:
                preload_hint["byterange_length"] = c_data.preload_hint_byterange_length
            data["preload_hint"] = preload_hint

        # Content steering
        if c_data.has_content_steering:
            content_steering = {
                "server_uri": _decode(c_data.content_steering_server_uri),
            }
            pathway_id = _decode(c_data.content_steering_pathway_id)
            if pathway_id is not None:
                content_steering["pathway_id"] = pathway_id
            data["content_steering"] = content_steering

        # Ensure keys list includes None for unencrypted segments
        # This matches the behavior of the Python parser
        has_unencrypted = any("key" not in seg for seg in data["segments"])
        if has_unencrypted and data["segments"]:
            # Insert None at the beginning of the keys list
            if None not in data["keys"]:
                data["keys"].insert(0, None)

        return data

    finally:
        # Always free C memory
        _lib.m3u8_free(c_data)


# Try to import the compiled C extension
try:
    from m3u8._m3u8_cparser import ffi as _ffi, lib as _lib
except ImportError:
    # C extension not available, set sentinel values
    _ffi = None
    _lib = None

    def parse(content: str, strict: bool = False, custom_tags_parser=None) -> dict:
        """Fall back to Python parser when C extension is not available."""
        from m3u8.parser import parse as py_parse

        return py_parse(content, strict=strict, custom_tags_parser=custom_tags_parser)
