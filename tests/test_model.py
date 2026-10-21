# Copyright 2014 Globo.com Player authors. All rights reserved.
# Modifications Copyright (c) 2026 Wurl.
# Use of this source code is governed by a MIT License
# license that can be found in the LICENSE file.

# Tests M3U8 class to make sure all attributes and methods use the correct
# data returned from parser.parse()

import datetime
import os
import textwrap
from contextlib import chdir
from tempfile import TemporaryDirectory
from unittest import TestCase

import openm3u8 as m3u8
from openm3u8.model import (
    DateRange,
    Key,
    Media,
    MediaList,
    PartialSegment,
    PreloadHint,
    RenditionReport,
    Segment,
    SessionData,
    denormalize_attribute,
    find_key,
)
from openm3u8.protocol import ext_x_part, ext_x_preload_hint, ext_x_start
from tests import playlists

utc = datetime.timezone.utc


class ModelTests(TestCase):
    def test_base_path_playlist_with_slash_in_query_string(self):
        playlist = m3u8.M3U8(
            playlists.PLAYLIST_WITH_SLASH_IN_QUERY_STRING,
            base_path="http://testvideo.com/foo",
        )
        self.assertEqual(
            playlist.segments[0].uri,
            "http://testvideo.com/foo/testvideo-1596635509-4769390994-a0e3087c.ts?hdntl=exp=1596678764~acl=/*~data=hdntl~hmac=12345&",
        )

    def test_target_duration_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"targetduration": "1234567"})

        self.assertEqual(obj.target_duration, "1234567")

    def test_media_sequence_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"media_sequence": 1234567})

        self.assertEqual(obj.media_sequence, 1234567)

    def test_program_date_time_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_PROGRAM_DATE_TIME)

        self.assertEqual(
            obj.program_date_time,
            datetime.datetime(2014, 8, 13, 13, 36, 33, tzinfo=utc),
        )

    def test_program_date_time_attribute_for_each_segment(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_PROGRAM_DATE_TIME)

        first_program_date_time = datetime.datetime(2014, 8, 13, 13, 36, 33, tzinfo=utc)

        # first segment contains both program_date_time and current_program_date_time
        self.assertEqual(obj.segments[0].program_date_time, first_program_date_time)
        self.assertEqual(
            obj.segments[0].current_program_date_time, first_program_date_time
        )

        # other segments contain only current_program_date_time
        for idx, segment in enumerate(obj.segments[1:]):
            self.assertIsNone(segment.program_date_time)
            self.assertEqual(
                segment.current_program_date_time,
                first_program_date_time + datetime.timedelta(seconds=(idx + 1) * 3),
            )

    def test_program_date_time_attribute_with_discontinuity(self):
        obj = m3u8.M3U8(playlists.DISCONTINUITY_PLAYLIST_WITH_PROGRAM_DATE_TIME)

        first_program_date_time = datetime.datetime(2014, 8, 13, 13, 36, 33, tzinfo=utc)
        discontinuity_program_date_time = datetime.datetime(
            2014, 8, 13, 13, 36, 55, tzinfo=utc
        )

        segments = obj.segments

        # first segment has EXT-X-PROGRAM-DATE-TIME
        self.assertEqual(segments[0].program_date_time, first_program_date_time)
        self.assertEqual(segments[0].current_program_date_time, first_program_date_time)

        # second segment does not have EXT-X-PROGRAM-DATE-TIME
        self.assertIsNone(segments[1].program_date_time)
        self.assertEqual(
            segments[1].current_program_date_time,
            first_program_date_time + datetime.timedelta(seconds=3),
        )

        # segment with EXT-X-DISCONTINUITY also has EXT-X-PROGRAM-DATE-TIME
        self.assertEqual(segments[5].program_date_time, discontinuity_program_date_time)
        self.assertEqual(
            segments[5].current_program_date_time, discontinuity_program_date_time
        )

        # subsequent segment does not have EXT-X-PROGRAM-DATE-TIME
        self.assertEqual(
            segments[6].current_program_date_time,
            discontinuity_program_date_time + datetime.timedelta(seconds=3),
        )
        self.assertIsNone(segments[6].program_date_time)

    def test_program_date_time_attribute_without_discontinuity(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_PROGRAM_DATE_TIME_WITHOUT_DISCONTINUITY)

        first_program_date_time = datetime.datetime(2019, 6, 10, 0, 5, tzinfo=utc)

        for idx, segment in enumerate(obj.segments):
            program_date_time = first_program_date_time + datetime.timedelta(
                seconds=idx * 6
            )
            self.assertEqual(segment.program_date_time, program_date_time)
            self.assertEqual(segment.current_program_date_time, program_date_time)

    def test_segment_discontinuity_attribute(self):
        obj = m3u8.M3U8(playlists.DISCONTINUITY_PLAYLIST_WITH_PROGRAM_DATE_TIME)
        segments = obj.segments

        self.assertIs(segments[0].discontinuity, False)
        self.assertIs(segments[5].discontinuity, True)
        self.assertIs(segments[6].discontinuity, False)

    def test_segment_cue_out_attribute(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_PLAYLIST)
        segments = obj.segments

        self.assertIs(segments[1].cue_out, True)
        self.assertIs(segments[2].cue_out, True)
        self.assertIs(segments[3].cue_out, False)

    def test_segment_cue_out_start_attribute(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_WITH_DURATION_PLAYLIST)

        self.assertIs(obj.segments[0].cue_out_start, True)

    def test_segment_cue_in_attribute(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_WITH_DURATION_PLAYLIST)

        self.assertIs(obj.segments[2].cue_in, True)

    def test_segment_cue_out_cont_dumps(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_PLAYLIST)

        result = obj.dumps()
        expected = "#EXT-X-CUE-OUT-CONT\n"
        self.assertIn(expected, result)

    def test_segment_cue_out_cont_attributes_dumps(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_ELEMENTAL_PLAYLIST)

        result = obj.dumps()
        expected = (
            "#EXT-X-CUE-OUT-CONT:"
            "ElapsedTime=7.960,"
            "Duration=50,SCTE35=/DAlAAAAAAAAAP/wFAUAAAABf+//wpiQkv4ARKogAAEBAQAAQ6sodg==\n"
        )
        self.assertIn(expected, result)

    def test_segment_oatcls_scte35_non_cue_out_dumps(self):
        obj = m3u8.M3U8(playlists.OATCLS_ELEMENTAL_PLAYLIST)
        result = obj.dumps()

        # Check OATCLS-SCTE35 for non-CUE-OUT lines
        cue_out_line = "/DAqAAAAAyiYAP/wBQb/FuaKGAAUAhJDVUVJAAAFp3+/EQMCRgIMAQF7Ny4D\n"
        self.assertEqual(result.count(cue_out_line), 1)

    def test_oatcls_extended(self):
        obj = m3u8.M3U8(playlists.OATCLS_EXTENDED_PLAYLIST)

        actual = []
        for i, line in enumerate(obj.dumps().splitlines()):
            if line.startswith("#EXT-OATCLS-SCTE35:"):
                actual.append((i, line[19:]))

        expected = [
            (6, "/DAlAAAAAAAAAP/wFAUbus/gf+/+UsdkQv4ApPWwAAEBAQAAj5YQOw=="),
            (
                17,
                "/DA7AAAAAAAAAP/wBQb+U2xZ8gAlAiNDVUVJ92lrqX+/DBRBTUNOQU1DTlZSMDAwMDA2NjYwMhAAAJi60Tw=",
            ),
        ]
        self.assertEqual(actual, expected)

    def test_segment_cue_out_start_dumps(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_WITH_DURATION_PLAYLIST)

        result = obj.dumps()
        expected = "#EXT-X-CUE-OUT:11.52\n"
        self.assertIn(expected, result)

    def test_segment_cue_out_start_explicit_dumps(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_WITH_EXPLICIT_DURATION_PLAYLIST)

        result = obj.dumps()
        expected = "#EXT-X-CUE-OUT:DURATION=11.52\n"
        self.assertIn(expected, result)

    def test_segment_cue_out_start_no_duration_dumps(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_NO_DURATION_PLAYLIST)

        result = obj.dumps()
        expected = "#EXT-X-CUE-OUT\n"
        self.assertIn(expected, result)

    def test_segment_cue_out_in_dumps(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_NO_DURATION_PLAYLIST)

        result = obj.dumps()
        expected = "#EXT-X-CUE-IN\n"
        self.assertIn(expected, result)

    def test_segment_elemental_scte35_attribute(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_ELEMENTAL_PLAYLIST)
        segments = obj.segments
        self.assertIs(segments[4].cue_out, True)
        self.assertIs(segments[9].cue_out, False)
        self.assertEqual(
            segments[4].scte35,
            "/DAlAAAAAAAAAP/wFAUAAAABf+//wpiQkv4ARKogAAEBAQAAQ6sodg==",
        )

    def test_segment_cue_out_cont_alt(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_CONT_ALT_PLAYLIST)
        segments = obj.segments

        self.assertEqual(segments[1].scte35_elapsedtime, "2")
        self.assertEqual(segments[1].scte35_duration, "120")

        self.assertEqual(segments[2].scte35_elapsedtime, "8")
        self.assertEqual(segments[2].scte35_duration, "120.0")

        self.assertEqual(segments[3].scte35_elapsedtime, "14.001")
        self.assertEqual(segments[3].scte35_duration, "120.0")

    def test_segment_cue_out_cont_mediaconvert(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_MEDIACONVERT_PLAYLIST)
        segments = obj.segments

        self.assertEqual(segments[2].scte35_elapsedtime, "10")
        self.assertEqual(segments[2].scte35_duration, "4")

    def test_segment_envivio_scte35_attribute(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_ENVIVIO_PLAYLIST)
        segments = obj.segments
        self.assertIs(segments[3].cue_out, True)
        self.assertEqual(
            segments[4].scte35,
            "/DAlAAAENOOQAP/wFAUBAABrf+//N25XDf4B9p/gAAEBAQAAxKni9A==",
        )
        self.assertEqual(
            segments[5].scte35,
            "/DAlAAAENOOQAP/wFAUBAABrf+//N25XDf4B9p/gAAEBAQAAxKni9A==",
        )
        self.assertIs(segments[7].cue_out, False)

    def test_segment_unknown_scte35_attribute(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_INVALID_PLAYLIST)
        self.assertIsNone(obj.segments[0].scte35)
        self.assertEqual(obj.segments[0].scte35_duration, "INVALID")

    def test_segment_cue_out_no_duration(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_NO_DURATION_PLAYLIST)
        self.assertIs(obj.segments[0].cue_out_start, True)
        self.assertIs(obj.segments[2].cue_in, True)

    def test_segment_asset_metadata_dumps(self):
        obj = m3u8.M3U8(playlists.CUE_OUT_ELEMENTAL_PLAYLIST)
        result = obj.dumps()

        # Only insert EXT-X-ASSET at cue out
        asset_metadata_line = (
            '#EXT-X-ASSET:GENRE=CV,CAID=12345678,EPISODE="Episode%20Name%20Date",'
            'SEASON="Season%20Name%20and%20Number",SERIES="Series%2520Name"\n'
        )
        self.assertEqual(result.count(asset_metadata_line), 1)

    def test_keys_on_clear_playlist(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)

        self.assertEqual(len(obj.keys), 1)
        self.assertIsNone(obj.keys[0])

    def test_keys_on_simple_encrypted_playlist(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS)

        self.assertEqual(len(obj.keys), 1)
        self.assertEqual(obj.keys[0].uri, "https://priv.example.com/key.php?r=52")

    def test_key_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        data = {"keys": [{"method": "AES-128", "uri": "/key", "iv": "foobar"}]}
        mock_parser_data(obj, data)

        self.assertEqual(obj.keys[0].__class__.__name__, "Key")
        self.assertEqual(obj.keys[0].method, "AES-128")
        self.assertEqual(obj.keys[0].uri, "/key")
        self.assertEqual(obj.keys[0].iv, "foobar")

    def test_key_attribute_on_none(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {})

        self.assertEqual(len(obj.keys), 0)

    def test_key_attribute_without_initialization_vector(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"keys": [{"method": "AES-128", "uri": "/key"}]})

        self.assertEqual(obj.keys[0].method, "AES-128")
        self.assertEqual(obj.keys[0].uri, "/key")
        self.assertIsNone(obj.keys[0].iv)

    def test_session_keys_on_clear_playlist(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)

        self.assertEqual(len(obj.session_keys), 0)

    def test_session_keys_on_simple_encrypted_playlist(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_SESSION_ENCRYPTED_SEGMENTS)

        self.assertEqual(len(obj.session_keys), 1)
        self.assertEqual(
            obj.session_keys[0].uri, "https://priv.example.com/key.php?r=52"
        )

    def test_session_key_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        data = {"session_keys": [{"method": "AES-128", "uri": "/key", "iv": "foobar"}]}
        mock_parser_data(obj, data)

        self.assertEqual(obj.session_keys[0].__class__.__name__, "SessionKey")
        self.assertEqual(obj.session_keys[0].method, "AES-128")
        self.assertEqual(obj.session_keys[0].uri, "/key")
        self.assertEqual(obj.session_keys[0].iv, "foobar")

    def test_session_key_attribute_on_none(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {})

        self.assertEqual(len(obj.session_keys), 0)

    def test_session_key_attribute_without_initialization_vector(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"session_keys": [{"method": "AES-128", "uri": "/key"}]})

        self.assertEqual(obj.session_keys[0].method, "AES-128")
        self.assertEqual(obj.session_keys[0].uri, "/key")
        self.assertIsNone(obj.session_keys[0].iv)

    def test_parse_tag_name_matches_fully(self):
        self.assertTrue(m3u8.M3U8(playlists.PLAYLIST_WITH_TAG_MEDIA_READY))

    def test_segments_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(
            obj,
            {
                "segments": [
                    {
                        "uri": "/foo/bar-1.ts",
                        "title": "First Segment",
                        "duration": 1500,
                    },
                    {
                        "uri": "/foo/bar-2.ts",
                        "title": "Second Segment",
                        "duration": 1600,
                    },
                ]
            },
        )

        self.assertEqual(len(obj.segments), 2)

        self.assertEqual(obj.segments[0].uri, "/foo/bar-1.ts")
        self.assertEqual(obj.segments[0].title, "First Segment")
        self.assertEqual(obj.segments[0].duration, 1500)

    def test_segments_attribute_without_title(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(
            obj, {"segments": [{"uri": "/foo/bar-1.ts", "duration": 1500}]}
        )

        self.assertEqual(len(obj.segments), 1)

        self.assertEqual(obj.segments[0].uri, "/foo/bar-1.ts")
        self.assertEqual(obj.segments[0].duration, 1500)
        self.assertIsNone(obj.segments[0].title)

    def test_segments_attribute_without_duration(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(
            obj, {"segments": [{"uri": "/foo/bar-1.ts", "title": "Segment title"}]}
        )

        self.assertEqual(len(obj.segments), 1)

        self.assertEqual(obj.segments[0].uri, "/foo/bar-1.ts")
        self.assertEqual(obj.segments[0].title, "Segment title")
        self.assertIsNone(obj.segments[0].duration)

    def test_segments_attribute_with_byterange(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(
            obj,
            {
                "segments": [
                    {
                        "uri": "/foo/bar-1.ts",
                        "title": "Segment title",
                        "duration": 1500,
                        "byterange": "76242@0",
                    }
                ]
            },
        )

        self.assertEqual(len(obj.segments), 1)

        self.assertEqual(obj.segments[0].uri, "/foo/bar-1.ts")
        self.assertEqual(obj.segments[0].title, "Segment title")
        self.assertEqual(obj.segments[0].duration, 1500)
        self.assertEqual(obj.segments[0].byterange, "76242@0")

    def test_segment_attribute_with_multiple_keys(self):
        obj = m3u8.M3U8(
            playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV_WITH_MULTIPLE_KEYS
        )

        segments = obj.segments
        self.assertEqual(segments[0].key.uri, "/hls-key/key.bin")
        self.assertEqual(segments[1].key.uri, "/hls-key/key.bin")
        self.assertEqual(segments[4].key.uri, "/hls-key/key2.bin")
        self.assertEqual(segments[5].key.uri, "/hls-key/key2.bin")

    def test_segment_title_dumps(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_QUOTED_TITLE)

        result = obj.segments[0].dumps(None).strip()
        expected = '#EXTINF:5220,"A sample title"\nhttp://media.example.com/entire.ts'

        self.assertEqual(result, expected)

    def test_is_variant_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"is_variant": False})
        self.assertFalse(obj.is_variant)

        mock_parser_data(obj, {"is_variant": True})
        self.assertTrue(obj.is_variant)

    def test_is_endlist_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"is_endlist": False})
        self.assertFalse(obj.is_endlist)

        obj = m3u8.M3U8(playlists.SLIDING_WINDOW_PLAYLIST)
        mock_parser_data(obj, {"is_endlist": True})
        self.assertTrue(obj.is_endlist)

    def test_is_i_frames_only_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"is_i_frames_only": False})
        self.assertFalse(obj.is_i_frames_only)

        mock_parser_data(obj, {"is_i_frames_only": True})
        self.assertTrue(obj.is_i_frames_only)

    def test_playlists_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        data = {
            "playlists": [
                {
                    "uri": "/url/1.m3u8",
                    "stream_info": {
                        "program_id": 1,
                        "bandwidth": 320000,
                        "closed_captions": None,
                        "video": "high",
                    },
                },
                {
                    "uri": "/url/2.m3u8",
                    "stream_info": {
                        "program_id": 1,
                        "bandwidth": 120000,
                        "closed_captions": None,
                        "codecs": "mp4a.40.5",
                        "video": "low",
                    },
                },
            ],
            "media": [
                {"type": "VIDEO", "name": "High", "group_id": "high"},
                {
                    "type": "VIDEO",
                    "name": "Low",
                    "group_id": "low",
                    "default": "YES",
                    "autoselect": "YES",
                },
            ],
        }
        mock_parser_data(obj, data)

        self.assertEqual(len(obj.playlists), 2)

        self.assertEqual(obj.playlists[0].uri, "/url/1.m3u8")
        self.assertEqual(obj.playlists[0].stream_info.program_id, 1)
        self.assertEqual(obj.playlists[0].stream_info.bandwidth, 320000)
        self.assertIsNone(obj.playlists[0].stream_info.closed_captions)
        self.assertIsNone(obj.playlists[0].stream_info.codecs)

        self.assertIsNone(obj.playlists[0].media[0].uri)
        self.assertEqual(obj.playlists[0].media[0].group_id, "high")
        self.assertEqual(obj.playlists[0].media[0].type, "VIDEO")
        self.assertIsNone(obj.playlists[0].media[0].language)
        self.assertEqual(obj.playlists[0].media[0].name, "High")
        self.assertIsNone(obj.playlists[0].media[0].default)
        self.assertIsNone(obj.playlists[0].media[0].autoselect)
        self.assertIsNone(obj.playlists[0].media[0].forced)
        self.assertIsNone(obj.playlists[0].media[0].characteristics)

        self.assertEqual(obj.playlists[1].uri, "/url/2.m3u8")
        self.assertEqual(obj.playlists[1].stream_info.program_id, 1)
        self.assertEqual(obj.playlists[1].stream_info.bandwidth, 120000)
        self.assertIsNone(obj.playlists[1].stream_info.closed_captions)
        self.assertEqual(obj.playlists[1].stream_info.codecs, "mp4a.40.5")

        self.assertIsNone(obj.playlists[1].media[0].uri)
        self.assertEqual(obj.playlists[1].media[0].group_id, "low")
        self.assertEqual(obj.playlists[1].media[0].type, "VIDEO")
        self.assertIsNone(obj.playlists[1].media[0].language)
        self.assertEqual(obj.playlists[1].media[0].name, "Low")
        self.assertEqual(obj.playlists[1].media[0].default, "YES")
        self.assertEqual(obj.playlists[1].media[0].autoselect, "YES")
        self.assertIsNone(obj.playlists[1].media[0].forced)
        self.assertIsNone(obj.playlists[1].media[0].characteristics)

        self.assertEqual(obj.iframe_playlists, [])

    def test_playlists_attribute_without_program_id(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(
            obj,
            {
                "playlists": [
                    {"uri": "/url/1.m3u8", "stream_info": {"bandwidth": 320000}}
                ]
            },
        )

        self.assertEqual(len(obj.playlists), 1)

        self.assertEqual(obj.playlists[0].uri, "/url/1.m3u8")
        self.assertEqual(obj.playlists[0].stream_info.bandwidth, 320000)
        self.assertIsNone(obj.playlists[0].stream_info.codecs)
        self.assertIsNone(obj.playlists[0].stream_info.program_id)

    def test_playlists_attribute_with_resolution(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_RESOLUTION)

        self.assertEqual(len(obj.playlists), 2)
        self.assertEqual(obj.playlists[0].stream_info.resolution, (512, 288))
        self.assertIsNone(obj.playlists[1].stream_info.resolution)

    def test_iframe_playlists_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        data = {
            "iframe_playlists": [
                {
                    "uri": "/url/1.m3u8",
                    "iframe_stream_info": {
                        "program_id": 1,
                        "bandwidth": 320000,
                        "resolution": "320x180",
                        "codecs": "avc1.4d001f",
                    },
                },
                {
                    "uri": "/url/2.m3u8",
                    "iframe_stream_info": {
                        "bandwidth": "120000",
                        "codecs": "avc1.4d400d",
                    },
                },
            ]
        }
        mock_parser_data(obj, data)

        self.assertEqual(len(obj.iframe_playlists), 2)

        self.assertEqual(obj.iframe_playlists[0].uri, "/url/1.m3u8")
        self.assertEqual(obj.iframe_playlists[0].iframe_stream_info.program_id, 1)
        self.assertEqual(obj.iframe_playlists[0].iframe_stream_info.bandwidth, 320000)
        self.assertEqual(
            obj.iframe_playlists[0].iframe_stream_info.resolution, (320, 180)
        )
        self.assertEqual(
            obj.iframe_playlists[0].iframe_stream_info.codecs, "avc1.4d001f"
        )

        self.assertEqual(obj.iframe_playlists[1].uri, "/url/2.m3u8")
        self.assertIsNone(obj.iframe_playlists[1].iframe_stream_info.program_id)
        self.assertEqual(obj.iframe_playlists[1].iframe_stream_info.bandwidth, "120000")
        self.assertIsNone(obj.iframe_playlists[1].iframe_stream_info.resolution)
        self.assertEqual(
            obj.iframe_playlists[1].iframe_stream_info.codecs, "avc1.4d400d"
        )

    def test_version_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"version": 2})
        self.assertEqual(obj.version, 2)

        mock_parser_data(obj, {})
        self.assertIsNone(obj.version)

    def test_version_settable_as_int(self):
        obj = m3u8.loads(playlists.VERSION_PLAYLIST)
        obj.version = 9

        self.assertIn("#EXT-X-VERSION:9", obj.dumps().strip())

    def test_version_settable_as_string(self):
        obj = m3u8.loads(playlists.VERSION_PLAYLIST)
        obj.version = "9"

        self.assertIn("#EXT-X-VERSION:9", obj.dumps().strip())

    def test_allow_cache_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        mock_parser_data(obj, {"allow_cache": "no"})
        self.assertEqual(obj.allow_cache, "no")

        mock_parser_data(obj, {})
        self.assertIsNone(obj.allow_cache)

    def test_files_attribute_should_list_all_files_including_segments_and_key(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS)
        files = [
            "https://priv.example.com/key.php?r=52",
            "http://media.example.com/fileSequence52-1.ts",
            "http://media.example.com/fileSequence52-2.ts",
            "http://media.example.com/fileSequence52-3.ts",
        ]
        self.assertEqual(obj.files, files)

    def test_vod_playlist_type_should_be_imported_as_a_simple_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_VOD_PLAYLIST_TYPE)
        self.assertEqual(obj.playlist_type, "vod")

    def test_event_playlist_type_should_be_imported_as_a_simple_attribute(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_EVENT_PLAYLIST_TYPE)
        self.assertEqual(obj.playlist_type, "event")

    def test_independent_segments_should_be_true(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_INDEPENDENT_SEGMENTS)
        self.assertTrue(obj.is_independent_segments)

    def test_independent_segments_should_be_false(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_EVENT_PLAYLIST_TYPE)
        self.assertFalse(obj.is_independent_segments)

    def test_no_playlist_type_leaves_attribute_empty(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        self.assertIsNone(obj.playlist_type)

    def test_dump_playlists_with_resolution(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_RESOLUTION)

        expected = playlists.SIMPLE_PLAYLIST_WITH_RESOLUTION.strip().splitlines()

        self.assertEqual(obj.dumps().strip().splitlines(), expected)

    def test_dump_should_build_file_with_same_content(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV)

        expected = playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV_SORTED.replace(
            ", IV", ",IV"
        ).strip()
        with TemporaryDirectory() as tmpdir:
            filename = os.path.join(tmpdir, "playlist.m3u8")
            obj.dump(filename)
            self.assert_file_content(filename, expected)

    def test_dump_should_create_sub_directories(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV)

        expected = playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV_SORTED.replace(
            ", IV", ",IV"
        ).strip()
        with TemporaryDirectory() as tmpdir:
            filename = os.path.join(tmpdir, "subdir1", "subdir2", "playlist.m3u8")
            obj.dump(filename)
            self.assert_file_content(filename, expected)

    def test_dump_should_raise_if_create_sub_directories_fails(self):
        with TemporaryDirectory() as tmpdir:
            # The first subdirectory is read-only
            subdir_1 = os.path.join(tmpdir, "subdir1")
            os.mkdir(subdir_1, mode=0o400)
            try:
                # The file is to be stored in a second subdirectory that's underneath the first
                subdir_2 = os.path.join(subdir_1, "subdir2")
                file_name = os.path.join(subdir_2, "playlist.m3u8")

                # When we try to write it, we'll be prevented from creating the second subdirectory
                with self.assertRaises(OSError):
                    m3u8.M3U8(playlists.SIMPLE_PLAYLIST).dump(file_name)
            finally:
                os.chmod(subdir_1, 0o700)

    def test_create_sub_directories_with_relative_path(self):
        relative_path = os.path.join("relative", "path", "playlist.m3u8")

        with TemporaryDirectory() as tmpdir:
            # Use a temporary directory as the current working directory for the test
            with chdir(tmpdir):
                obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
                obj.dump(relative_path)

            expected_file_path = os.path.join(tmpdir, relative_path)
            self.assertTrue(os.path.exists(expected_file_path))

            with open(expected_file_path) as file:
                self.assertEqual(file.read().strip(), playlists.SIMPLE_PLAYLIST.strip())

    def test_dump_should_work_for_variant_streams(self):
        obj = m3u8.M3U8(playlists.VARIANT_PLAYLIST)

        expected = playlists.VARIANT_PLAYLIST.replace(
            ", BANDWIDTH", ",BANDWIDTH"
        ).strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_dump_should_work_for_variant_playlists_with_iframe_playlists(self):
        obj = m3u8.M3U8(playlists.VARIANT_PLAYLIST_WITH_IFRAME_PLAYLISTS)

        expected = playlists.VARIANT_PLAYLIST_WITH_IFRAME_PLAYLISTS.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_dump_should_work_for_iframe_playlists(self):
        obj = m3u8.M3U8(playlists.IFRAME_PLAYLIST)

        expected = playlists.IFRAME_PLAYLIST.strip()

        self.assertEqual(obj.dumps().strip(), expected)

        obj = m3u8.M3U8(playlists.IFRAME_PLAYLIST2)

        expected = playlists.IFRAME_PLAYLIST.strip()

        # expected that dump will reverse EXTINF and EXT-X-BYTERANGE,
        # hence IFRAME_PLAYLIST dump from IFRAME_PLAYLIST2 parse.
        self.assertEqual(obj.dumps().strip(), expected)

        obj = m3u8.M3U8(playlists.IFRAME_PLAYLIST2)

        expected = playlists.IFRAME_PLAYLIST.strip()

        # expected that dump will reverse EXTINF and EXT-X-BYTERANGE,
        # hence IFRAME_PLAYLIST dump from IFRAME_PLAYLIST2 parse.
        self.assertEqual(obj.dumps().strip(), expected)

    def test_dump_should_include_program_date_time(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_PROGRAM_DATE_TIME)

        self.assertIn(
            "EXT-X-PROGRAM-DATE-TIME:2014-08-13T13:36:33.000+00:00",
            obj.dumps().strip(),
        )

    def test_dump_segment_honors_timespec(self):
        segment = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_PROGRAM_DATE_TIME).segments[
            0
        ]
        segment_text = segment.dumps(None, timespec="microseconds").strip()

        self.assertIn(
            "EXT-X-PROGRAM-DATE-TIME:2014-08-13T13:36:33.000000+00:00", segment_text
        )

    def test_dump_honors_timespec(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_PROGRAM_DATE_TIME)
        obj_text = obj.dumps(timespec="microseconds").strip()

        self.assertIn(
            "EXT-X-PROGRAM-DATE-TIME:2014-08-13T13:36:33.000000+00:00", obj_text
        )

    def test_dump_should_not_ignore_zero_duration(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_ZERO_DURATION)

        self.assertIn("EXTINF:0", obj.dumps().strip())
        self.assertIn("EXTINF:5220", obj.dumps().strip())

        self.assertIn("EXTINF:0.000", obj.dumps(infspec="milliseconds").strip())
        self.assertIn("EXTINF:5220.000", obj.dumps(infspec="milliseconds").strip())

        self.assertIn("EXTINF:0.000000", obj.dumps(infspec="microseconds").strip())
        self.assertIn("EXTINF:5220.000000", obj.dumps(infspec="microseconds").strip())

    def test_dump_should_use_decimal_floating_point_for_very_short_durations(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_VERY_SHORT_DURATION)

        self.assertIn("EXTINF:5220", obj.dumps().strip())
        self.assertIn("EXTINF:5218.5", obj.dumps().strip())
        self.assertIn("EXTINF:0.000011", obj.dumps().strip())

        self.assertIn("EXTINF:5220.000", obj.dumps(infspec="milliseconds").strip())
        self.assertIn("EXTINF:5218.500", obj.dumps(infspec="milliseconds").strip())
        self.assertIn("EXTINF:0.000", obj.dumps(infspec="milliseconds").strip())

        self.assertIn("EXTINF:5220.000000", obj.dumps(infspec="microseconds").strip())
        self.assertIn("EXTINF:5218.500", obj.dumps(infspec="microseconds").strip())
        self.assertIn("EXTINF:0.000011", obj.dumps(infspec="microseconds").strip())

    def test_dump_should_include_segment_level_program_date_time(self):
        obj = m3u8.M3U8(playlists.DISCONTINUITY_PLAYLIST_WITH_PROGRAM_DATE_TIME)
        # Tag being expected is in the segment level, not the global one
        self.assertIn(
            "#EXT-X-PROGRAM-DATE-TIME:2014-08-13T13:36:55.000+00:00",
            obj.dumps().strip(),
        )

    def test_dump_should_include_segment_level_program_date_time_without_discontinuity(
        self,
    ):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_PROGRAM_DATE_TIME_WITHOUT_DISCONTINUITY)

        output = obj.dumps().strip()
        self.assertIn("#EXT-X-PROGRAM-DATE-TIME:2019-06-10T00:05:00.000+00:00", output)
        self.assertIn("#EXT-X-PROGRAM-DATE-TIME:2019-06-10T00:05:06.000+00:00", output)
        self.assertIn("#EXT-X-PROGRAM-DATE-TIME:2019-06-10T00:05:12.000+00:00", output)

    def test_dump_should_include_map_attributes(self):
        obj = m3u8.M3U8(playlists.MAP_URI_PLAYLIST_WITH_BYTERANGE)

        self.assertIn('EXT-X-MAP:URI="main.mp4",BYTERANGE="812@0"', obj.dumps().strip())

    def test_multiple_map_attributes(self):
        obj = m3u8.M3U8(playlists.MULTIPLE_MAP_URI_PLAYLIST)

        self.assertEqual(obj.segments[0].init_section.uri, "init1.mp4")
        self.assertEqual(obj.segments[1].init_section.uri, "init1.mp4")
        self.assertEqual(obj.segments[2].init_section.uri, "init3.mp4")

    def test_empty_map_tag(self):
        obj = m3u8.M3U8(playlists.EMPTY_MAP_TAG_PLAYLIST)

        # Empty MAP tag should not cause errors and segments should have no init_section
        self.assertIsNone(obj.segments[0].init_section)
        self.assertIsNone(obj.segments[1].init_section)

    def test_dump_should_include_multiple_map_attributes(self):
        obj = m3u8.M3U8(playlists.MULTIPLE_MAP_URI_PLAYLIST)

        with TemporaryDirectory() as tmpdir:
            obj.dump(os.path.join(tmpdir, "d.m3u8"))
        output = obj.dumps().strip()
        self.assertEqual(output.count('#EXT-X-MAP:URI="init1.mp4"'), 1)
        self.assertEqual(output.count('#EXT-X-MAP:URI="init3.mp4"'), 1)

    def test_dump_should_work_for_playlists_using_byteranges(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_USING_BYTERANGES)

        expected = playlists.PLAYLIST_USING_BYTERANGES.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_dump_with_endlist_tag(self):
        obj = m3u8.M3U8(playlists.SLIDING_WINDOW_PLAYLIST)
        obj.is_endlist = True

        self.assertIn("#EXT-X-ENDLIST", obj.dumps().splitlines())

    def test_should_dump_without_endlist_tag(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST)
        obj.is_endlist = False

        expected = playlists.SIMPLE_PLAYLIST.strip().splitlines()
        expected.remove("#EXT-X-ENDLIST")

        self.assertEqual(obj.dumps().strip().splitlines(), expected)

    def test_should_dump_multiple_keys(self):
        obj = m3u8.M3U8(
            playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV_WITH_MULTIPLE_KEYS
        )
        expected = playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV_WITH_MULTIPLE_KEYS_SORTED.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_dump_unencrypted_encrypted_keys_together(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED)
        expected = (
            playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED.strip()
        )

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_dump_complex_unencrypted_encrypted_keys(self):
        obj = m3u8.M3U8(
            playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED_NONE
        )
        expected = playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED_NONE.replace(
            'METHOD=NONE,URI=""', "METHOD=NONE"
        ).strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_dump_complex_unencrypted_encrypted_keys_no_uri_attr(self):
        obj = m3u8.M3U8(
            playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED_NONE_AND_NO_URI_ATTR
        )
        expected = playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED_NONE_AND_NO_URI_ATTR.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_dump_session_data(self):
        obj = m3u8.M3U8(playlists.SESSION_DATA_PLAYLIST)
        expected = playlists.SESSION_DATA_PLAYLIST.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_dump_multiple_session_data(self):
        obj = m3u8.M3U8(playlists.MULTIPLE_SESSION_DATA_PLAYLIST)
        expected = playlists.MULTIPLE_SESSION_DATA_PLAYLIST.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_length_segments_by_key(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED)

        self.assertEqual(len(obj.segments.by_key(obj.keys[0])), 2)
        self.assertEqual(len(obj.segments.by_key(obj.keys[1])), 4)
        self.assertEqual(len(obj.segments.by_key(obj.keys[2])), 2)

    def test_list_segments_by_key(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED)

        # unencrypted segments
        segments = obj.segments.by_key(None)
        expected = (
            "../../../../hls/streamNum82400.ts\n../../../../hls/streamNum82401.ts"
        )
        output = [segment.uri for segment in segments]
        self.assertEqual("\n".join(output).strip(), expected.strip())

        # segments for last key
        segments = obj.segments.by_key(obj.keys[2])
        expected = (
            "../../../../hls/streamNum82404.ts\n../../../../hls/streamNum82405.ts"
        )
        output = [segment.uri for segment in segments]
        self.assertEqual("\n".join(output).strip(), expected.strip())

    def test_replace_segment_key(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED)

        # Replace unencrypted segments with new key
        new_key = Key(
            "AES-128",
            None,
            "/hls-key/key0.bin",
            iv="0Xcafe8f758ca555115584bb5b3c687f52",
        )
        for segment in obj.segments.by_key(None):
            segment.key = new_key

        # Check dump
        expected = playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED_UPDATED.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_keyformat_and_keyformatversion(self):
        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_KEYFORMAT_AND_KEYFORMATVERSIONS)

        result = obj.dumps().strip()
        expected = 'KEYFORMAT="com.apple.streamingkeydelivery",KEYFORMATVERSIONS="1"'

        self.assertIn(expected, result)

    def test_should_dump_program_datetime_and_discontinuity(self):
        obj = m3u8.M3U8(playlists.DISCONTINUITY_PLAYLIST_WITH_PROGRAM_DATE_TIME)
        expected = playlists.DISCONTINUITY_PLAYLIST_WITH_PROGRAM_DATE_TIME.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_normalize_segments_and_key_urls_if_base_path_passed_to_constructor(
        self,
    ):
        base_path = "http://videoserver.com/hls/live"

        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV, base_path)

        self.assertEqual(obj.base_path, base_path)

        expected = (
            playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV_SORTED.replace(
                ", IV", ",IV"
            )
            .replace("../../../../hls", base_path)
            .replace("/hls-key", base_path)
            .strip()
        )

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_normalize_session_key_urls_if_base_path_passed_to_constructor(
        self,
    ):
        base_path = "http://videoserver.com/hls/live"

        obj = m3u8.M3U8(
            playlists.PLAYLIST_WITH_SESSION_ENCRYPTED_SEGMENTS_AND_IV, base_path
        )

        self.assertEqual(obj.base_path, base_path)

        expected = (
            playlists.PLAYLIST_WITH_SESSION_ENCRYPTED_SEGMENTS_AND_IV_SORTED.replace(
                ", IV", ",IV"
            )
            .replace("../../../../hls", base_path)
            .replace("/hls-key", base_path)
            .strip()
        )

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_normalize_variant_streams_urls_if_base_path_passed_to_constructor(
        self,
    ):
        base_path = "http://videoserver.com/hls/live"
        obj = m3u8.M3U8(playlists.VARIANT_PLAYLIST, base_path)

        expected = (
            playlists.VARIANT_PLAYLIST.replace(", BANDWIDTH", ",BANDWIDTH")
            .replace("http://example.com", base_path)
            .strip()
        )

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_normalize_segments_and_key_urls_if_base_path_attribute_updated(
        self,
    ):
        base_path = "http://videoserver.com/hls/live"

        obj = m3u8.M3U8(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV)
        obj.base_path = base_path

        expected = (
            playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV_SORTED.replace(
                ", IV", ",IV"
            )
            .replace("../../../../hls", base_path)
            .replace("/hls-key", base_path)
            .strip()
        )

        self.assertEqual(obj.dumps().strip(), expected)

    def test_playlist_type_dumped_to_appropriate_m3u8_field(self):
        obj = m3u8.M3U8()
        obj.playlist_type = "vod"
        result = obj.dumps()
        expected = "#EXTM3U\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        self.assertEqual(result, expected)

    def test_empty_playlist_type_is_gracefully_ignored(self):
        obj = m3u8.M3U8()
        obj.playlist_type = ""
        result = obj.dumps()
        expected = "#EXTM3U\n"
        self.assertEqual(result, expected)

    def test_none_playlist_type_is_gracefully_ignored(self):
        obj = m3u8.M3U8()
        obj.playlist_type = None
        result = obj.dumps()
        expected = "#EXTM3U\n"
        self.assertEqual(result, expected)

    def test_0_media_sequence_added_to_file(self):
        obj = m3u8.M3U8()
        obj.media_sequence = 0
        result = obj.dumps()
        expected = "#EXTM3U\n"
        self.assertEqual(result, expected)

    def test_none_media_sequence_gracefully_ignored(self):
        obj = m3u8.M3U8()
        obj.media_sequence = None
        result = obj.dumps()
        expected = "#EXTM3U\n"
        self.assertEqual(result, expected)

    def test_0_discontinuity_sequence_added_to_file(self):
        obj = m3u8.M3U8()
        obj.discontinuity_sequence = 0
        result = obj.dumps()
        expected = "#EXTM3U\n"
        self.assertEqual(result, expected)

    def test_none_discontinuity_sequence_gracefully_ignored(self):
        obj = m3u8.M3U8()
        obj.discontinuity_sequence = None
        result = obj.dumps()
        expected = "#EXTM3U\n"
        self.assertEqual(result, expected)

    def test_non_zero_discontinuity_sequence_added_to_file(self):
        obj = m3u8.M3U8()
        obj.discontinuity_sequence = 1
        result = obj.dumps()
        expected = "#EXT-X-DISCONTINUITY-SEQUENCE:1"
        self.assertIn(expected, result)

    def test_should_correctly_update_base_path_if_its_blank(self):
        segment = Segment("entire.ts", "http://1.2/")
        self.assertFalse(segment.base_path)
        segment.base_path = "base_path"
        self.assertEqual(segment.absolute_uri, "http://1.2/base_path/entire.ts")

    def test_base_path_should_just_return_uri_if_absolute(self):
        segment = Segment("http://1.2/entire.ts", "")
        self.assertEqual(segment.absolute_uri, "http://1.2/entire.ts")

    def test_m3u8_should_propagate_base_uri_to_segments(self):
        with open(playlists.RELATIVE_PLAYLIST_FILENAME) as f:
            content = f.read()
        obj = m3u8.M3U8(content, base_uri="/any/path")
        self.assertEqual(obj.segments[0].uri, "/entire1.ts")
        self.assertEqual(obj.segments[0].absolute_uri, "/entire1.ts")
        self.assertEqual(obj.segments[3].uri, "entire4.ts")
        self.assertEqual(obj.segments[3].absolute_uri, "/any/path/entire4.ts")
        obj.base_uri = "/any/where/"
        self.assertEqual(obj.segments[0].uri, "/entire1.ts")
        self.assertEqual(obj.segments[0].absolute_uri, "/entire1.ts")
        self.assertEqual(obj.segments[3].uri, "entire4.ts")
        self.assertEqual(obj.segments[3].absolute_uri, "/any/where/entire4.ts")

    def test_m3u8_should_propagate_base_uri_to_key(self):
        with open(playlists.RELATIVE_PLAYLIST_FILENAME) as f:
            content = f.read()
        obj = m3u8.M3U8(content, base_uri="/any/path")
        self.assertEqual(obj.keys[0].uri, "../key.bin")
        self.assertEqual(obj.keys[0].absolute_uri, "/any/key.bin")
        obj.base_uri = "/any/where/"
        self.assertEqual(obj.keys[0].uri, "../key.bin")
        self.assertEqual(obj.keys[0].absolute_uri, "/any/key.bin")

    def test_m3u8_should_propagate_base_uri_to_session_key(self):
        with open(playlists.RELATIVE_PLAYLIST_FILENAME) as f:
            content = f.read()
        obj = m3u8.M3U8(content, base_uri="/any/path")
        self.assertEqual(obj.session_keys[0].uri, "../key.bin")
        self.assertEqual(obj.session_keys[0].absolute_uri, "/any/key.bin")
        obj.base_uri = "/any/where/"
        self.assertEqual(obj.session_keys[0].uri, "../key.bin")
        self.assertEqual(obj.session_keys[0].absolute_uri, "/any/key.bin")

    def test_base_path_with_optional_uri_should_do_nothing(self):
        media = Media(type="AUDIO", group_id="audio-group", name="English")
        self.assertIsNone(media.uri)
        self.assertIsNone(media.base_uri)
        media.base_path = "base_path"
        self.assertIsNone(media.absolute_uri)
        self.assertIsNone(media.base_path)

    def test_medialist_uri_method(self):
        langs = ["English", "French", "German"]
        ml = MediaList()
        for lang in langs:
            ml.append(
                Media(
                    type="AUDIO",
                    group_id="audio-group",
                    name=lang,
                    uri=f"/{lang}.m3u8",
                )
            )

        self.assertEqual(len(ml.uri), len(langs))
        self.assertEqual(ml.uri[0], f"/{langs[0]}.m3u8")
        self.assertEqual(ml.uri[1], f"/{langs[1]}.m3u8")
        self.assertEqual(ml.uri[2], f"/{langs[2]}.m3u8")

    def test_segment_map_uri_attribute(self):
        obj = m3u8.M3U8(playlists.MAP_URI_PLAYLIST)
        self.assertEqual(obj.segment_map[0].uri, "fileSequence0.mp4")

    def test_segment_map_uri_attribute_with_byterange(self):
        obj = m3u8.M3U8(playlists.MAP_URI_PLAYLIST_WITH_BYTERANGE)
        self.assertEqual(obj.segment_map[0].uri, "main.mp4")
        self.assertEqual(obj.segment_map[0].byterange, "812@0")
        self.assertEqual(obj.segment_map[1].uri, "main2.mp4")
        self.assertEqual(obj.segment_map[1].byterange, "912@0")

    def test_start_with_negative_offset(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_START_NEGATIVE_OFFSET)
        self.assertEqual(obj.start.time_offset, -2.0)
        self.assertIsNone(obj.start.precise)
        self.assertIn(ext_x_start + ":TIME-OFFSET=-2.0\n", obj.dumps())

    def test_start_with_precise(self):
        obj = m3u8.M3U8(playlists.SIMPLE_PLAYLIST_WITH_START_PRECISE)
        self.assertEqual(obj.start.time_offset, 10.5)
        self.assertEqual(obj.start.precise, "YES")
        self.assertIn(ext_x_start + ":TIME-OFFSET=10.5,PRECISE=YES\n", obj.dumps())

    def test_playlist_stream_info_contains_group_id_refs(self):
        obj = m3u8.M3U8(playlists.VARIANT_PLAYLIST_WITH_VIDEO_CC_SUBS_AND_AUDIO)
        self.assertEqual(len(obj.playlists), 2)
        for pl in obj.playlists:
            self.assertEqual(pl.stream_info.closed_captions, '"cc"')
            self.assertEqual(pl.stream_info.subtitles, "sub")
            self.assertEqual(pl.stream_info.audio, "aud")
            self.assertEqual(pl.stream_info.video, "vid")

    def test_should_dump_frame_rate(self):
        obj = m3u8.M3U8(playlists.VARIANT_PLAYLIST_WITH_FRAME_RATE)
        expected = playlists.VARIANT_PLAYLIST_WITH_FRAME_RATE.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_should_round_frame_rate(self):
        obj = m3u8.M3U8(playlists.VARIANT_PLAYLIST_WITH_ROUNDABLE_FRAME_RATE)
        expected = playlists.VARIANT_PLAYLIST_WITH_ROUNDED_FRAME_RATE.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_add_segment_to_playlist(self):
        obj = m3u8.M3U8()

        obj.add_segment(Segment("entire.ts", "http://1.2/", duration=1))

    def test_segment_str_method(self):
        segment = Segment("entire.ts", "http://1.2/", duration=1)

        expected = "#EXTINF:1,\nentire.ts"
        result = str(segment).strip()

        self.assertEqual(result, expected)

    def test_attribute_denormaliser(self):
        result = denormalize_attribute("test_test")
        expected = "TEST-TEST"

        self.assertEqual(result, expected)

    def test_find_key_throws_when_no_match(self):
        with self.assertRaises(KeyError):
            find_key(
                {"method": "AES-128", "iv": 0x12345678, "uri": "http://1.2/"},
                [
                    # deliberately empty
                ],
            )

    def test_ll_playlist(self):
        obj = m3u8.M3U8(playlists.LOW_LATENCY_DELTA_UPDATE_PLAYLIST)
        obj.base_path = "http://localhost/test_base_path"
        obj.base_uri = "http://localhost/test_base_uri"

        self.assertEqual(len(obj.rendition_reports), 2)
        self.assertEqual(len(obj.segments[2].parts), 12)
        self.assertIn(
            ext_x_part
            + ':DURATION=0.33334,URI="http://localhost/test_base_path/filePart271.0.ts"',
            obj.dumps(),
        )
        self.assertIn(
            ext_x_preload_hint
            + ':TYPE=PART,URI="http://localhost/test_base_path/filePart273.4.ts"',
            obj.dumps(),
        )
        self.assertEqual(obj.preload_hint.base_uri, "http://localhost/test_base_uri")

    def test_ll_playlist_omitted_attributes(self):
        # RFC 8216 4.4.5.4 states that even the required attribute LAST-MSN
        # can be omitted in certain conditions.
        obj = m3u8.M3U8(playlists.LOW_LATENCY_OMITTED_ATTRIBUTES)
        text = obj.dumps()
        self.assertIn('#EXT-X-RENDITION-REPORT:URI="rendition_1.m3u8"\n', text)

    def test_add_rendition_report_to_playlist(self):
        obj = m3u8.M3U8()

        obj.add_rendition_report(
            RenditionReport(
                base_uri=None, uri="../1M/waitForMSN.php", last_msn=273, last_part=0
            )
        )

        obj.base_path = "http://localhost/test"

        result = obj.dumps()
        expected = '#EXT-X-RENDITION-REPORT:URI="http://localhost/test/waitForMSN.php",LAST-MSN=273,LAST-PART=0'

        self.assertIn(expected, result)

    def test_add_part_to_segment(self):
        obj = Segment(uri="fileSequence271.ts", duration=4.00008)

        obj.add_part(PartialSegment(None, "filePart271.0.ts", 0.33334))

        result = obj.dumps(None)
        expected = '#EXT-X-PART:DURATION=0.33334,URI="filePart271.0.ts"'

        self.assertIn(expected, result)

    def test_partial_segment_gap_and_byterange(self):
        obj = PartialSegment(
            "", "filePart271.0.ts", 0.33334, byterange="9400@376", gap="YES"
        )

        result = obj.dumps(None)
        expected = '#EXT-X-PART:DURATION=0.33334,URI="filePart271.0.ts",BYTERANGE=9400@376,GAP=YES'

        self.assertEqual(result, expected)

    def test_session_data_with_value(self):
        obj = SessionData("com.example.value", "example", language="en")

        result = obj.dumps()
        expected = '#EXT-X-SESSION-DATA:DATA-ID="com.example.value",VALUE="example",LANGUAGE="en"'

        self.assertEqual(result, expected)

    def test_session_data_with_uri(self):
        obj = SessionData("com.example.value", uri="example.json", language="en")

        result = obj.dumps()
        expected = '#EXT-X-SESSION-DATA:DATA-ID="com.example.value",URI="example.json",LANGUAGE="en"'

        self.assertEqual(result, expected)

    def test_session_data_cannot_be_created_with_value_and_uri_at_the_same_time(self):
        obj = SessionData(
            "com.example.value", value="example", uri="example.json", language="en"
        )

        result = obj.dumps()
        expected = '#EXT-X-SESSION-DATA:DATA-ID="com.example.value",VALUE="example",LANGUAGE="en"'

        self.assertEqual(result, expected)

    def test_endswith_newline(self):
        obj = m3u8.loads(playlists.SIMPLE_PLAYLIST)

        manifest = obj.dumps()

        self.assertTrue(manifest.endswith("#EXT-X-ENDLIST\n"))

    def test_init_section_base_path_update(self):
        obj = m3u8.M3U8(playlists.MULTIPLE_MAP_URI_PLAYLIST)

        self.assertEqual(obj.segments[0].init_section.uri, "init1.mp4")

        obj.base_path = "http://localhost/base_path"
        obj.base_uri = "http://localhost/base_uri"

        self.assertEqual(
            obj.segments[0].init_section.uri, "http://localhost/base_path/init1.mp4"
        )
        self.assertEqual(
            obj.segments[0].init_section.base_uri, "http://localhost/base_uri"
        )

    def test_iframe_playlists_base_path_update(self):
        obj = m3u8.M3U8(playlists.VARIANT_PLAYLIST_WITH_IFRAME_PLAYLISTS)

        self.assertEqual(obj.iframe_playlists[0].uri, "video-800k-iframes.m3u8")
        self.assertIsNone(obj.iframe_playlists[0].base_uri)

        obj.base_path = "http://localhost/base_path"
        obj.base_uri = "http://localhost/base_uri"

        self.assertEqual(
            obj.iframe_playlists[0].uri,
            "http://localhost/base_path/video-800k-iframes.m3u8",
        )
        self.assertEqual(obj.iframe_playlists[0].base_uri, "http://localhost/base_uri")

    def test_partial_segment_base_path_update(self):
        obj = m3u8.M3U8(playlists.LOW_LATENCY_DELTA_UPDATE_PLAYLIST)

        obj.base_path = "http://localhost/base_path"
        obj.base_uri = "http://localhost/base_uri"

        self.assertEqual(
            obj.segments[2].parts[0].uri, "http://localhost/base_path/filePart271.0.ts"
        )
        self.assertEqual(obj.segments[2].parts[0].base_uri, "http://localhost/base_uri")

    def test_add_preload_hint(self):
        obj = PreloadHint("PART", "", "filePart273.4.ts", 0)

        result = obj.dumps()
        expected = (
            '#EXT-X-PRELOAD-HINT:TYPE=PART,URI="filePart273.4.ts",BYTERANGE-START=0'
        )

        self.assertEqual(result, expected)

    def test_add_daterange(self):
        obj = DateRange(
            id="testid123",
            start_date="2020-03-09T17:19:00Z",
            planned_duration=60,
            x_test_client_attr='"test-attr"',
        )

        result = obj.dumps()
        expected = '#EXT-X-DATERANGE:ID="testid123",START-DATE="2020-03-09T17:19:00Z",PLANNED-DURATION=60,X-TEST-CLIENT-ATTR="test-attr"'

        self.assertEqual(result, expected)

    def test_daterange_simple(self):
        obj = m3u8.M3U8(playlists.DATERANGE_SIMPLE_PLAYLIST)

        # note that x-<client-attribute>s are explicitly alphabetically ordered
        # when dumped for predictability, so line below is different from input
        expected = '#EXT-X-DATERANGE:ID="ad3",START-DATE="2016-06-13T11:15:00Z",DURATION=20,X-AD-ID="1234",X-AD-URL="http://ads.example.com/beacon3"'
        result = obj.dumps()

        self.assertIn(expected, result)

    def test_daterange_scte_out_and_in(self):
        obj = m3u8.M3U8(playlists.DATERANGE_SCTE35_OUT_AND_IN_PLAYLIST)

        result = obj.dumps()

        daterange_out = '#EXT-X-DATERANGE:ID="splice-6FFFFFF0",START-DATE="2014-03-05T11:15:00Z",PLANNED-DURATION=59.993,SCTE35-OUT=0xFC002F0000000000FF000014056FFFFFF000E011622DCAFF000052636200000000000A0008029896F50000008700000000'
        daterange_in = '#EXT-X-DATERANGE:ID="splice-6FFFFFF0",DURATION=59.993,SCTE35-IN=0xFC002A0000000000FF00000F056FFFFFF000401162802E6100000000000A0008029896F50000008700000000'

        self.assertIn(daterange_out, result)
        self.assertIn(daterange_in, result)

    def test_daterange_enddate_sctecmd(self):
        obj = m3u8.M3U8(playlists.DATERANGE_ENDDATE_SCTECMD_PLAYLIST)

        result = obj.dumps()
        expected = '#EXT-X-DATERANGE:ID="test_id",START-DATE="2020-03-11T10:51:00Z",CLASS="test_class",END-DATE="2020-03-11T10:52:00Z",DURATION=60,SCTE35-CMD=0xFCINVALIDSECTION'

        self.assertIn(expected, result)

    def test_daterange_in_parts(self):
        obj = m3u8.M3U8(playlists.DATERANGE_IN_PART_PLAYLIST)

        result = obj.dumps()
        expected = '#EXT-X-DATERANGE:ID="test_id",START-DATE="2020-03-10T07:48:02Z",CLASS="test_class",END-ON-NEXT=YES'

        self.assertIn(expected, result)

    def test_add_gap(self):
        obj = m3u8.Segment(uri="fileSequence271.ts", duration=4, gap_tag=True)

        result = str(obj)
        expected = "#EXTINF:4,\n#EXT-X-GAP\nfileSequence271.ts"

        self.assertEqual(result, expected)

    def test_gap(self):
        obj = m3u8.M3U8(playlists.GAP_PLAYLIST)

        result = obj.dumps().strip()
        expected = playlists.GAP_PLAYLIST.strip()

        self.assertEqual(result, expected)

    def test_gap_in_parts(self):
        obj = m3u8.M3U8(playlists.GAP_IN_PARTS_PLAYLIST)

        result = obj.dumps().strip()
        expected = playlists.GAP_IN_PARTS_PLAYLIST.strip()

        self.assertEqual(result, expected)

    def test_add_blackout_simple(self):
        obj = m3u8.Segment(uri="segment.ts", duration=4, blackout=True)

        result = str(obj)
        expected = "#EXT-X-BLACKOUT\n#EXTINF:4,\nsegment.ts"

        self.assertEqual(result, expected)

    def test_add_blackout_with_params(self):
        obj = m3u8.Segment(
            uri="segment.ts", duration=4, blackout="ID=12345,DURATION=30.0"
        )

        result = str(obj)
        expected = "#EXT-X-BLACKOUT:ID=12345,DURATION=30.0\n#EXTINF:4,\nsegment.ts"

        self.assertEqual(result, expected)

    def test_blackout_simple(self):
        obj = m3u8.M3U8(playlists.BLACKOUT_SIMPLE_PLAYLIST)

        result = obj.dumps().strip()
        expected = playlists.BLACKOUT_SIMPLE_PLAYLIST.strip()

        self.assertEqual(result, expected)

    def test_blackout_with_params(self):
        obj = m3u8.M3U8(playlists.BLACKOUT_WITH_PARAMS_PLAYLIST)

        result = obj.dumps().strip()
        expected = playlists.BLACKOUT_WITH_PARAMS_PLAYLIST.strip()

        self.assertEqual(result, expected)

    def test_skip_dateranges(self):
        obj = m3u8.M3U8(playlists.DELTA_UPDATE_SKIP_DATERANGES_PLAYLIST)

        expected_skip_tag = (
            '#EXT-X-SKIP:SKIPPED-SEGMENTS=16,RECENTLY-REMOVED-DATERANGES="1"'
        )
        expected_server_control_tag = (
            "#EXT-X-SERVER-CONTROL:CAN-SKIP-UNTIL=36,CAN-SKIP-DATERANGES=YES"
        )

        result = obj.dumps().strip()

        self.assertIn(expected_skip_tag, result)
        self.assertIn(expected_server_control_tag, result)

    def test_add_skip(self):
        obj = m3u8.Skip(skipped_segments=30, recently_removed_dateranges="1\t2")

        expected = '#EXT-X-SKIP:SKIPPED-SEGMENTS=30,RECENTLY-REMOVED-DATERANGES="1\t2"'
        result = obj.dumps().strip()

        self.assertEqual(result, expected)

    def test_content_steering(self):
        obj = m3u8.M3U8(playlists.CONTENT_STEERING_PLAYLIST)

        expected_content_steering_tag = '#EXT-X-CONTENT-STEERING:SERVER-URI="/steering?video=00012",PATHWAY-ID="CDN-A"'
        result = obj.dumps().strip()

        self.assertIn(expected_content_steering_tag, result)

    def test_add_content_steering(self):
        obj = m3u8.ContentSteering("", "/steering?video=00012", "CDN-A")

        expected = '#EXT-X-CONTENT-STEERING:SERVER-URI="/steering?video=00012",PATHWAY-ID="CDN-A"'
        result = obj.dumps().strip()

        self.assertEqual(result, expected)

    def test_content_steering_base_path_update(self):
        obj = m3u8.M3U8(playlists.CONTENT_STEERING_PLAYLIST)
        obj.base_path = "https://another.example.com/"

        self.assertIn(
            '#EXT-X-CONTENT-STEERING:SERVER-URI="https://another.example.com/steering?video=00012",PATHWAY-ID="CDN-A"',
            obj.dumps().strip(),
        )

    def test_add_content_steering_base_uri_update(self):
        obj = m3u8.M3U8(playlists.CONTENT_STEERING_PLAYLIST)
        obj.base_uri = "https://yet-another.example.com/"

        self.assertEqual(
            obj.content_steering.absolute_uri,
            "https://yet-another.example.com/steering?video=00012",
        )

    def test_dump_should_work_for_variant_playlists_with_image_playlists(self):
        obj = m3u8.M3U8(playlists.VARIANT_PLAYLIST_WITH_IMAGE_PLAYLISTS)

        expected = playlists.VARIANT_PLAYLIST_WITH_IMAGE_PLAYLISTS.strip()

        self.assertEqual(obj.dumps().strip(), expected)

    def test_segment_media_sequence(self):
        obj = m3u8.M3U8(playlists.SLIDING_WINDOW_PLAYLIST)
        self.assertEqual([s.media_sequence for s in obj.segments], [2680, 2681, 2682])

    def test_low_latency_output(self):
        obj = m3u8.M3U8(playlists.LOW_LATENCY_PART_PLAYLIST)
        actual = obj.dumps()
        expected = textwrap.dedent(
            """\
            #EXTM3U
            #EXT-X-MEDIA-SEQUENCE:264
            #EXT-X-VERSION:6
            #EXT-X-TARGETDURATION:4
            #EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,PART-HOLD-BACK=1,CAN-SKIP-UNTIL=24
            #EXT-X-PART-INF:PART-TARGET=0.33334
            #EXT-X-MAP:URI="init.mp4"
            #EXT-X-PROGRAM-DATE-TIME:2019-02-14T02:13:28.106+00:00
            #EXTINF:4.00008,
            fileSequence264.mp4
            #EXTINF:4.00008,
            fileSequence265.mp4
            #EXTINF:4.00008,
            fileSequence266.mp4
            #EXTINF:4.00008,
            fileSequence267.mp4
            #EXTINF:4.00008,
            fileSequence268.mp4
            #EXTINF:4.00008,
            fileSequence269.mp4
            #EXTINF:4.00008,
            fileSequence270.mp4
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.0.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.1.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.2.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.3.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.4.mp4",INDEPENDENT=YES
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.5.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.6.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.7.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.8.mp4",INDEPENDENT=YES
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.9.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.10.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart271.11.mp4"
            #EXTINF:4.00008,
            fileSequence271.mp4
            #EXT-X-PROGRAM-DATE-TIME:2019-02-14T02:14:00.106+00:00
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.a.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.b.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.c.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.d.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.e.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.f.mp4",INDEPENDENT=YES
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.g.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.h.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.i.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.j.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.k.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart272.l.mp4"
            #EXTINF:4.00008,
            fileSequence272.mp4
            #EXT-X-PART:DURATION=0.33334,URI="filePart273.0.mp4",INDEPENDENT=YES
            #EXT-X-PART:DURATION=0.33334,URI="filePart273.1.mp4"
            #EXT-X-PART:DURATION=0.33334,URI="filePart273.2.mp4"

            #EXT-X-PRELOAD-HINT:TYPE=PART,URI="filePart273.3.mp4"
            #EXT-X-RENDITION-REPORT:URI="../1M/waitForMSN.php",LAST-MSN=273,LAST-PART=2
            #EXT-X-RENDITION-REPORT:URI="../4M/waitForMSN.php",LAST-MSN=273,LAST-PART=1
            """
        )
        self.assertEqual(actual, expected)

    def test_bitrate_settable_as_int(self):
        obj = m3u8.loads(playlists.BITRATE_PLAYLIST)
        obj.segments[0].bitrate = 9876

        self.assertIn("#EXT-X-BITRATE:9876", obj.dumps().strip())

    def assert_file_content(self, filename, expected):
        with open(filename) as fileobj:
            content = fileobj.read().strip()

        self.assertEqual(content, expected)


# helpers


def mock_parser_data(m3u8_obj, data):
    data.setdefault("segments", [])
    m3u8_obj.data = data
    m3u8_obj._initialize_attributes()
