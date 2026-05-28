# Copyright 2014 Globo.com Player authors. All rights reserved.
# Modifications Copyright (c) 2026 Wurl.
# Use of this source code is governed by a MIT License
# license that can be found in the LICENSE file.

import itertools
import re
from datetime import datetime, timedelta

try:
    from backports.datetime_fromisoformat import MonkeyPatch

    MonkeyPatch.patch_fromisoformat()
except ImportError:
    pass


from openm3u8 import protocol, version_matching

"""
http://tools.ietf.org/html/draft-pantos-http-live-streaming-08#section-3.2
http://stackoverflow.com/questions/2785755/how-to-split-but-ignore-separators-in-quoted-strings-in-python
"""
ATTRIBUTELISTPATTERN = re.compile(r"""((?:[^,"']|"[^"]*"|'[^']*')+)""")


def cast_date_time(value):
    return datetime.fromisoformat(value)


def format_date_time(value, **kwargs):
    return value.isoformat(**kwargs)


class ParseError(Exception):
    def __init__(self, lineno, line):
        self.lineno = lineno
        self.line = line

    def __str__(self):
        return "Syntax error in manifest on line %d: %s" % (self.lineno, self.line)


def parse(content, strict=False, custom_tags_parser=None):
    """
    Given a M3U8 playlist content returns a dictionary with all data found
    """
    data = {
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

    state = {
        "expect_segment": False,
        "expect_playlist": False,
        "current_keys": [],
        "current_segment_map": None,
    }

    lines = string_to_lines(content)
    if strict:
        found_errors = version_matching.validate(lines)

        if len(found_errors) > 0:
            raise Exception(found_errors)

    for lineno, line in enumerate(lines, 1):
        line = line.strip()
        parse_kwargs = {
            "line": line,
            "lineno": lineno,
            "data": data,
            "state": state,
            "strict": strict,
        }

        # Blank lines are ignored.
        if not line:
            continue

        # Call custom parser if needed
        if line.startswith("#") and callable(custom_tags_parser):
            go_to_next_line = custom_tags_parser(line, lineno, data, state)

            # Do not try to parse other standard tags on this line if custom_tags_parser
            # function returns `True`
            if go_to_next_line:
                continue

        # Fast-path: dispatch based on tag token up to first ':' (or full tag if none)
        if line.startswith("#"):
            tag = line.split(":", 1)[0]
            handler = DISPATCH.get(tag)
            if handler is not None:
                handler(**parse_kwargs)
                continue
            # #EXTM3U should be present; ignore if seen
            if tag == protocol.ext_m3u:
                continue
            # In strict mode, unrecognized tags are illegal
            if strict:
                raise ParseError(lineno, line)
            continue

        # Lines that don't start with # are either segments or playlists.
        if state["expect_segment"]:
            _parse_ts_chunk(**parse_kwargs)
        elif state["expect_playlist"]:
            _parse_variant_playlist(**parse_kwargs)
        # In strict mode, any other content is illegal
        elif strict:
            raise ParseError(lineno, line)

    # Handle remaining partial segments.
    if "segment" in state:
        data["segments"].append(state.pop("segment"))

    return data


def _parse_key(line, data, state, **kwargs):
    params = ATTRIBUTELISTPATTERN.split(line.replace(protocol.ext_x_key + ":", ""))[
        1::2
    ]
    key = {}
    for param in params:
        name, value = param.split("=", 1)
        key[normalize_attribute(name)] = remove_quotes(value)

    # Per HLS spec: a key applies until the next key with the same KEYFORMAT
    # (or "identity" if no KEYFORMAT). Replace any existing key with matching
    # KEYFORMAT, or append if it's a new KEYFORMAT.
    # METHOD=NONE also gets stored (to distinguish explicit unencrypted from
    # never-encrypted), and it clears keys with the same KEYFORMAT.
    new_keyformat = key.get("keyformat", "identity")

    # Find and replace key with same KEYFORMAT, or append if not found
    replaced = False
    for i, existing in enumerate(state["current_keys"]):
        if existing.get("keyformat", "identity") == new_keyformat:
            state["current_keys"][i] = key
            replaced = True
            break
    if not replaced:
        state["current_keys"].append(key)

    if key not in data["keys"]:
        data["keys"].append(key)


def _parse_extinf(line, state, lineno, strict, **kwargs):
    chunks = line.replace(protocol.extinf + ":", "").split(",", 1)
    if len(chunks) == 2:
        duration, title = chunks
    elif len(chunks) == 1:
        if strict:
            raise ParseError(lineno, line)
        else:
            duration = chunks[0]
            title = ""
    if "segment" not in state:
        state["segment"] = {}
    state["segment"]["duration"] = float(duration)
    state["segment"]["title"] = title
    state["expect_segment"] = True


def _parse_ts_chunk(line, data, state, **kwargs):
    segment = state.pop("segment")
    if state.get("program_date_time"):
        segment["program_date_time"] = state.pop("program_date_time")
    if state.get("current_program_date_time"):
        segment["current_program_date_time"] = state["current_program_date_time"]
        state["current_program_date_time"] += timedelta(seconds=segment["duration"])
    segment["uri"] = line
    segment["cue_in"] = state.pop("cue_in", False)
    segment["cue_out"] = state.pop("cue_out", False)
    segment["cue_out_start"] = state.pop("cue_out_start", False)
    segment["cue_out_explicitly_duration"] = state.pop(
        "cue_out_explicitly_duration", False
    )

    scte_op = state.get if segment["cue_out"] else state.pop
    segment["scte35"] = scte_op("current_cue_out_scte35", None)
    segment["oatcls_scte35"] = scte_op("current_cue_out_oatcls_scte35", None)
    segment["scte35_duration"] = scte_op("current_cue_out_duration", None)
    segment["scte35_elapsedtime"] = scte_op("current_cue_out_elapsedtime", None)
    segment["asset_metadata"] = scte_op("asset_metadata", None)

    segment["discontinuity"] = state.pop("discontinuity", False)
    if state["current_keys"]:
        # Copy the list so modifications don't affect previous segments
        segment["keys"] = list(state["current_keys"])
    else:
        # For unencrypted segments, the initial key would be None
        if None not in data["keys"]:
            data["keys"].append(None)
    if state.get("current_segment_map"):
        segment["init_section"] = state["current_segment_map"]
    segment["dateranges"] = state.pop("dateranges", None)
    segment["gap_tag"] = state.pop("gap", None)
    segment["blackout"] = state.pop("blackout", None)
    data["segments"].append(segment)
    state["expect_segment"] = False


def _parse_attribute_list(prefix, line, attribute_parser, default_parser=None):
    params = ATTRIBUTELISTPATTERN.split(line.replace(prefix + ":", ""))[1::2]

    attributes = {}
    if not line.startswith(prefix + ":"):
        return attributes

    for param in params:
        param_parts = param.split("=", 1)
        if len(param_parts) == 1:
            name = ""
            value = param_parts[0]
        else:
            name, value = param_parts

        name = normalize_attribute(name)
        if name in attribute_parser:
            value = attribute_parser[name](value)
        elif default_parser is not None:
            value = default_parser(value)

        attributes[name] = value

    return attributes


def _parse_stream_inf(line, data, state, **kwargs):
    state["expect_playlist"] = True
    data["is_variant"] = True
    data["media_sequence"] = None
    state["stream_info"] = _parse_attribute_list(
        protocol.ext_x_stream_inf, line, STREAM_INF_ATTRIBUTE_PARSER
    )


def _parse_i_frame_stream_inf(line, data, **kwargs):
    iframe_stream_info = _parse_attribute_list(
        protocol.ext_x_i_frame_stream_inf, line, IFRAME_STREAM_INF_ATTRIBUTE_PARSER
    )
    iframe_playlist = {
        "uri": iframe_stream_info.pop("uri"),
        "iframe_stream_info": iframe_stream_info,
    }

    data["iframe_playlists"].append(iframe_playlist)


def _parse_image_stream_inf(line, data, **kwargs):
    image_stream_info = _parse_attribute_list(
        protocol.ext_x_image_stream_inf, line, IMAGE_STREAM_INF_ATTRIBUTE_PARSER
    )
    image_playlist = {
        "uri": image_stream_info.pop("uri"),
        "image_stream_info": image_stream_info,
    }

    data["image_playlists"].append(image_playlist)


def _parse_is_images_only(line, data, **kwargs):
    data["is_images_only"] = True


def _parse_tiles(line, data, state, **kwargs):
    tiles_info = _parse_attribute_list(
        protocol.ext_x_tiles, line, TILES_ATTRIBUTE_PARSER
    )
    data["tiles"].append(tiles_info)


def _parse_media(line, data, **kwargs):
    media = _parse_attribute_list(protocol.ext_x_media, line, MEDIA_ATTRIBUTE_PARSER)
    data["media"].append(media)


def _parse_variant_playlist(line, data, state, **kwargs):
    playlist = {"uri": line, "stream_info": state.pop("stream_info")}
    data["playlists"].append(playlist)
    state["expect_playlist"] = False


def _parse_bitrate(state, **kwargs):
    if "segment" not in state:
        state["segment"] = {}
    state["segment"]["bitrate"] = _parse_simple_parameter(cast_to=int, **kwargs)


def _parse_byterange(line, state, **kwargs):
    if "segment" not in state:
        state["segment"] = {}
    state["segment"]["byterange"] = line.replace(protocol.ext_x_byterange + ":", "")
    state["expect_segment"] = True


def _parse_targetduration(**parse_kwargs):
    return _parse_simple_parameter(cast_to=int, **parse_kwargs)


def _parse_media_sequence(**parse_kwargs):
    return _parse_simple_parameter(cast_to=int, **parse_kwargs)


def _parse_discontinuity_sequence(**parse_kwargs):
    return _parse_simple_parameter(cast_to=int, **parse_kwargs)


def _parse_program_date_time(line, state, data, **parse_kwargs):
    _, program_date_time = _parse_simple_parameter_raw_value(
        line, cast_to=cast_date_time, **parse_kwargs
    )
    if not data.get("program_date_time"):
        data["program_date_time"] = program_date_time
    state["current_program_date_time"] = program_date_time
    state["program_date_time"] = program_date_time


def _parse_discontinuity(state, **parse_kwargs):
    state["discontinuity"] = True


def _parse_cue_in(state, **parse_kwargs):
    state["cue_in"] = True


def _parse_cue_span(state, **parse_kwargs):
    state["cue_out"] = True


def _parse_version(**parse_kwargs):
    return _parse_simple_parameter(cast_to=int, **parse_kwargs)


def _parse_allow_cache(**parse_kwargs):
    return _parse_simple_parameter(cast_to=str, **parse_kwargs)


def _parse_playlist_type(line, data, **kwargs):
    return _parse_simple_parameter(line, data)


def _parse_x_map(line, data, state, **kwargs):
    segment_map_info = _parse_attribute_list(
        protocol.ext_x_map, line, X_MAP_ATTRIBUTE_PARSER
    )
    state["current_segment_map"] = segment_map_info
    data["segment_map"].append(segment_map_info)


def _parse_start(line, data, **kwargs):
    start_info = _parse_attribute_list(
        protocol.ext_x_start, line, START_ATTRIBUTE_PARSER
    )
    data["start"] = start_info


def _parse_gap(state, **kwargs):
    state["gap"] = True


def _parse_blackout(line, state, **kwargs):
    # Store the full tag content to pass through unmodified
    # Extract everything after "#EXT-X-BLACKOUT"
    if ":" in line:
        # Tag has parameters: #EXT-X-BLACKOUT:params
        blackout_data = line.split(":", 1)[1]
    else:
        # Tag has no parameters, just store True
        blackout_data = True
    state["blackout"] = blackout_data


def _parse_simple_parameter_raw_value(line, cast_to=str, normalize=False, **kwargs):
    param, value = line.split(":", 1)
    param = normalize_attribute(param.replace("#EXT-X-", ""))
    if normalize:
        value = value.strip().lower()
    return param, cast_to(value)


def _parse_and_set_simple_parameter_raw_value(
    line, data, cast_to=str, normalize=False, **kwargs
):
    param, value = _parse_simple_parameter_raw_value(line, cast_to, normalize)
    data[param] = value
    return data[param]


def _parse_simple_parameter(line, data, cast_to=str, **kwargs):
    return _parse_and_set_simple_parameter_raw_value(line, data, cast_to, True)


def _parse_i_frames_only(data, **kwargs):
    data["is_i_frames_only"] = True


def _parse_is_independent_segments(data, **kwargs):
    data["is_independent_segments"] = True


def _parse_endlist(data, **kwargs):
    data["is_endlist"] = True


def _parse_cueout_cont(line, state, **kwargs):
    state["cue_out"] = True

    elements = line.split(":", 1)
    if len(elements) != 2:
        return

    # EXT-X-CUE-OUT-CONT:ElapsedTime=10,Duration=60,SCTE35=... style
    cue_info = _parse_attribute_list(
        protocol.ext_x_cue_out_cont,
        line,
        CUEOUT_CONT_ATTRIBUTE_PARSER,
    )

    # EXT-X-CUE-OUT-CONT:2.436/120 style
    progress = cue_info.get("")
    if progress:
        progress_parts = progress.split("/", 1)
        if len(progress_parts) == 1:
            state["current_cue_out_duration"] = progress_parts[0]
        else:
            state["current_cue_out_elapsedtime"] = progress_parts[0]
            state["current_cue_out_duration"] = progress_parts[1]

    duration = cue_info.get("duration")
    if duration:
        state["current_cue_out_duration"] = duration

    scte35 = cue_info.get("scte35")
    if duration:
        state["current_cue_out_scte35"] = scte35

    elapsedtime = cue_info.get("elapsedtime")
    if elapsedtime:
        state["current_cue_out_elapsedtime"] = elapsedtime


def _parse_cueout(line, state, **kwargs):
    state["cue_out_start"] = True
    state["cue_out"] = True
    if "DURATION" in line.upper():
        state["cue_out_explicitly_duration"] = True

    elements = line.split(":", 1)
    if len(elements) != 2:
        return

    cue_info = _parse_attribute_list(
        protocol.ext_x_cue_out,
        line,
        CUEOUT_ATTRIBUTE_PARSER,
    )
    cue_out_scte35 = cue_info.get("cue")
    cue_out_duration = cue_info.get("duration") or cue_info.get("")

    current_cue_out_scte35 = state.get("current_cue_out_scte35")
    state["current_cue_out_scte35"] = cue_out_scte35 or current_cue_out_scte35
    state["current_cue_out_duration"] = cue_out_duration


def _parse_server_control(line, data, **kwargs):
    data["server_control"] = _parse_attribute_list(
        protocol.ext_x_server_control, line, SERVER_CONTROL_ATTRIBUTE_PARSER
    )


def _parse_part_inf(line, data, **kwargs):
    data["part_inf"] = _parse_attribute_list(
        protocol.ext_x_part_inf, line, PART_INF_ATTRIBUTE_PARSER
    )


def _parse_rendition_report(line, data, **kwargs):
    rendition_report = _parse_attribute_list(
        protocol.ext_x_rendition_report, line, RENDITION_REPORT_ATTRIBUTE_PARSER
    )

    data["rendition_reports"].append(rendition_report)


def _parse_part(line, state, **kwargs):
    part = _parse_attribute_list(protocol.ext_x_part, line, PART_ATTRIBUTE_PARSER)

    # this should always be true according to spec
    if state.get("current_program_date_time"):
        part["program_date_time"] = state["current_program_date_time"]
        state["current_program_date_time"] += timedelta(seconds=part["duration"])

    part["dateranges"] = state.pop("dateranges", None)
    part["gap_tag"] = state.pop("gap", None)

    if "segment" not in state:
        state["segment"] = {}
    segment = state["segment"]
    if "parts" not in segment:
        segment["parts"] = []

    segment["parts"].append(part)


def _parse_skip(line, data, **parse_kwargs):
    data["skip"] = _parse_attribute_list(
        protocol.ext_x_skip, line, SKIP_ATTRIBUTE_PARSER
    )


def _parse_session_data(line, data, **kwargs):
    session_data = _parse_attribute_list(
        protocol.ext_x_session_data, line, SESSION_DATA_ATTRIBUTE_PARSER
    )
    data["session_data"].append(session_data)


def _parse_session_key(line, data, **kwargs):
    params = ATTRIBUTELISTPATTERN.split(
        line.replace(protocol.ext_x_session_key + ":", "")
    )[1::2]
    key = {}
    for param in params:
        name, value = param.split("=", 1)
        key[normalize_attribute(name)] = remove_quotes(value)
    data["session_keys"].append(key)


def _parse_preload_hint(line, data, **kwargs):
    data["preload_hint"] = _parse_attribute_list(
        protocol.ext_x_preload_hint, line, PRELOAD_HINT_ATTRIBUTE_PARSER
    )


def _parse_daterange(line, state, **kwargs):
    parsed = _parse_attribute_list(
        protocol.ext_x_daterange, line, DATERANGE_ATTRIBUTE_PARSER
    )

    if "dateranges" not in state:
        state["dateranges"] = []

    state["dateranges"].append(parsed)


def _parse_content_steering(line, data, **kwargs):
    data["content_steering"] = _parse_attribute_list(
        protocol.ext_x_content_steering, line, CONTENT_STEERING_ATTRIBUTE_PARSER
    )


def _parse_oatcls_scte35(line, state, **kwargs):
    scte35_cue = line.split(":", 1)[1]
    state["current_cue_out_oatcls_scte35"] = scte35_cue
    if not state.get("current_cue_out_scte35"):
        state["current_cue_out_scte35"] = scte35_cue


def _parse_asset(line, state, **kwargs):
    # EXT-X-ASSET attribute values may or may not be quoted, and need to be URL-encoded.
    # They are preserved as-is here to prevent loss of information.
    state["asset_metadata"] = _parse_attribute_list(
        protocol.ext_x_asset, line, {}, default_parser=str
    )


def string_to_lines(string):
    return string.strip().splitlines()


def remove_quotes_parser(*attrs):
    return dict(zip(attrs, itertools.repeat(remove_quotes)))


def remove_quotes(string):
    """
    Remove quotes from string.

    Ex.:
      "foo" -> foo
      'foo' -> foo
      'foo  -> 'foo

    """
    quotes = ('"', "'")
    if string.startswith(quotes) and string.endswith(quotes):
        return string[1:-1]
    return string


def normalize_attribute(attribute):
    return attribute.replace("-", "_").lower().strip()


def get_segment_custom_value(state, key, default=None):
    """
    Helper function for getting custom values for Segment
    Are useful with custom_tags_parser
    """
    if "segment" not in state:
        return default
    if "custom_parser_values" not in state["segment"]:
        return default
    return state["segment"]["custom_parser_values"].get(key, default)


def save_segment_custom_value(state, key, value):
    """
    Helper function for saving custom values for Segment
    Are useful with custom_tags_parser
    """
    if "segment" not in state:
        state["segment"] = {}

    if "custom_parser_values" not in state["segment"]:
        state["segment"]["custom_parser_values"] = {}

    state["segment"]["custom_parser_values"][key] = value


# Attribute parser constants (built once)
STREAM_INF_ATTRIBUTE_PARSER = remove_quotes_parser(
    "codecs",
    "audio",
    "video",
    "video_range",
    "subtitles",
    "pathway_id",
    "stable_variant_id",
)
STREAM_INF_ATTRIBUTE_PARSER.update(
    {
        "program_id": int,
        "bandwidth": lambda x: int(float(x)),
        "average_bandwidth": int,
        "frame_rate": float,
        "hdcp_level": str,
    }
)

IFRAME_STREAM_INF_ATTRIBUTE_PARSER = remove_quotes_parser(
    "codecs", "uri", "pathway_id", "stable_variant_id"
)
IFRAME_STREAM_INF_ATTRIBUTE_PARSER.update(
    {
        "program_id": int,
        "bandwidth": int,
        "average_bandwidth": int,
        "hdcp_level": str,
    }
)

IMAGE_STREAM_INF_ATTRIBUTE_PARSER = remove_quotes_parser(
    "codecs", "uri", "pathway_id", "stable_variant_id"
)
IMAGE_STREAM_INF_ATTRIBUTE_PARSER.update(
    {
        "program_id": int,
        "bandwidth": int,
        "average_bandwidth": int,
        "resolution": str,
    }
)

MEDIA_ATTRIBUTE_PARSER = remove_quotes_parser(
    "uri",
    "group_id",
    "language",
    "assoc_language",
    "name",
    "instream_id",
    "characteristics",
    "channels",
    "stable_rendition_id",
    "thumbnails",
    "image",
)

X_MAP_ATTRIBUTE_PARSER = remove_quotes_parser("uri", "byterange")

START_ATTRIBUTE_PARSER = {"time_offset": lambda x: float(x)}

SERVER_CONTROL_ATTRIBUTE_PARSER = {
    "can_block_reload": str,
    "hold_back": lambda x: float(x),
    "part_hold_back": lambda x: float(x),
    "can_skip_until": lambda x: float(x),
    "can_skip_dateranges": str,
}

PART_INF_ATTRIBUTE_PARSER = {"part_target": lambda x: float(x)}

RENDITION_REPORT_ATTRIBUTE_PARSER = remove_quotes_parser("uri")
RENDITION_REPORT_ATTRIBUTE_PARSER.update({"last_msn": int, "last_part": int})

PART_ATTRIBUTE_PARSER = remove_quotes_parser("uri")
PART_ATTRIBUTE_PARSER.update(
    {"duration": lambda x: float(x), "independent": str, "gap": str, "byterange": str}
)

SKIP_ATTRIBUTE_PARSER = remove_quotes_parser("recently_removed_dateranges")
SKIP_ATTRIBUTE_PARSER.update({"skipped_segments": int})

SESSION_DATA_ATTRIBUTE_PARSER = remove_quotes_parser(
    "data_id", "value", "uri", "language"
)

PRELOAD_HINT_ATTRIBUTE_PARSER = remove_quotes_parser("uri")
PRELOAD_HINT_ATTRIBUTE_PARSER.update(
    {"type": str, "byterange_start": int, "byterange_length": int}
)

DATERANGE_ATTRIBUTE_PARSER = remove_quotes_parser(
    "id", "class", "start_date", "end_date"
)
DATERANGE_ATTRIBUTE_PARSER.update(
    {
        "duration": float,
        "planned_duration": float,
        "end_on_next": str,
        "scte35_cmd": str,
        "scte35_out": str,
        "scte35_in": str,
    }
)

CONTENT_STEERING_ATTRIBUTE_PARSER = remove_quotes_parser("server_uri", "pathway_id")

CUEOUT_CONT_ATTRIBUTE_PARSER = remove_quotes_parser("duration", "elapsedtime", "scte35")

CUEOUT_ATTRIBUTE_PARSER = remove_quotes_parser("cue")

TILES_ATTRIBUTE_PARSER = remove_quotes_parser("uri")
TILES_ATTRIBUTE_PARSER.update({"resolution": str, "layout": str, "duration": float})


# Single token-to-handler dispatch to avoid a long startswith chain
DISPATCH = {
    protocol.ext_x_byterange: _parse_byterange,
    protocol.ext_x_bitrate: _parse_bitrate,
    protocol.ext_x_targetduration: _parse_targetduration,
    protocol.ext_x_media_sequence: _parse_media_sequence,
    protocol.ext_x_discontinuity_sequence: _parse_discontinuity_sequence,
    protocol.ext_x_program_date_time: _parse_program_date_time,
    protocol.ext_x_discontinuity: _parse_discontinuity,
    protocol.ext_x_cue_out_cont: _parse_cueout_cont,
    protocol.ext_x_cue_out: _parse_cueout,
    protocol.ext_oatcls_scte35: _parse_oatcls_scte35,
    protocol.ext_x_asset: _parse_asset,
    protocol.ext_x_cue_in: _parse_cue_in,
    protocol.ext_x_cue_span: _parse_cue_span,
    protocol.ext_x_version: _parse_version,
    protocol.ext_x_allow_cache: _parse_allow_cache,
    protocol.ext_x_key: _parse_key,
    protocol.extinf: _parse_extinf,
    protocol.ext_x_stream_inf: _parse_stream_inf,
    protocol.ext_x_i_frame_stream_inf: _parse_i_frame_stream_inf,
    protocol.ext_x_media: _parse_media,
    protocol.ext_x_playlist_type: _parse_playlist_type,
    protocol.ext_i_frames_only: _parse_i_frames_only,
    protocol.ext_is_independent_segments: _parse_is_independent_segments,
    protocol.ext_x_endlist: _parse_endlist,
    protocol.ext_x_map: _parse_x_map,
    protocol.ext_x_start: _parse_start,
    protocol.ext_x_server_control: _parse_server_control,
    protocol.ext_x_part_inf: _parse_part_inf,
    protocol.ext_x_rendition_report: _parse_rendition_report,
    protocol.ext_x_part: _parse_part,
    protocol.ext_x_skip: _parse_skip,
    protocol.ext_x_session_data: _parse_session_data,
    protocol.ext_x_session_key: _parse_session_key,
    protocol.ext_x_preload_hint: _parse_preload_hint,
    protocol.ext_x_daterange: _parse_daterange,
    protocol.ext_x_gap: _parse_gap,
    protocol.ext_x_content_steering: _parse_content_steering,
    protocol.ext_x_image_stream_inf: _parse_image_stream_inf,
    protocol.ext_x_images_only: _parse_is_images_only,
    protocol.ext_x_tiles: _parse_tiles,
    protocol.ext_x_blackout: _parse_blackout,
}
