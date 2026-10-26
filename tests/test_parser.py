# Copyright 2014 Globo.com Player authors. All rights reserved.
# Modifications Copyright (c) 2026 Wurl.
# Use of this source code is governed by a MIT License
# license that can be found in the LICENSE file.
import re
from unittest import TestCase

import openm3u8 as m3u8
from openm3u8.parser import (
    ParseError,
    _parse_simple_parameter_raw_value,
    cast_date_time,
    get_segment_custom_value,
    save_segment_custom_value,
)
from tests import playlists


class ParserTests(TestCase):
    def test_should_parse_simple_playlist_from_string(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST)
        self.assertEqual(data["targetduration"], 5220)
        self.assertEqual(data["media_sequence"], 0)
        self.assertEqual(
            [c["uri"] for c in data["segments"]],
            ["http://media.example.com/entire.ts"],
        )
        self.assertEqual([c["duration"] for c in data["segments"]], [5220])

    def test_should_parse_non_integer_duration_from_playlist_string(self):
        data = m3u8.parse(playlists.PLAYLIST_WITH_NON_INTEGER_DURATION)
        self.assertEqual(data["targetduration"], 5221)
        self.assertEqual([c["duration"] for c in data["segments"]], [5220.5])

    def test_should_parse_comma_in_title(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_TITLE_COMMA)
        self.assertEqual(
            [c["title"] for c in data["segments"]], ["Title with a comma, end"]
        )

    def test_should_parse_simple_playlist_from_string_with_different_linebreaks(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST.replace("\n", "\r\n"))
        self.assertEqual(data["targetduration"], 5220)
        self.assertEqual(
            [c["uri"] for c in data["segments"]],
            ["http://media.example.com/entire.ts"],
        )
        self.assertEqual([c["duration"] for c in data["segments"]], [5220])

    def test_should_parse_sliding_window_playlist_from_string(self):
        data = m3u8.parse(playlists.SLIDING_WINDOW_PLAYLIST)
        self.assertEqual(data["targetduration"], 8)
        self.assertEqual(data["media_sequence"], 2680)
        self.assertEqual(
            [c["uri"] for c in data["segments"]],
            [
                "https://priv.example.com/fileSequence2680.ts",
                "https://priv.example.com/fileSequence2681.ts",
                "https://priv.example.com/fileSequence2682.ts",
            ],
        )
        self.assertEqual([c["duration"] for c in data["segments"]], [8, 8, 8])

    def test_should_parse_playlist_with_encrypted_segments_from_string(self):
        data = m3u8.parse(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS)
        self.assertEqual(data["media_sequence"], 7794)
        self.assertEqual(data["targetduration"], 15)
        self.assertEqual(data["keys"][0]["method"], "AES-128")
        self.assertEqual(
            data["keys"][0]["uri"], "https://priv.example.com/key.php?r=52"
        )
        self.assertEqual(
            [c["uri"] for c in data["segments"]],
            [
                "http://media.example.com/fileSequence52-1.ts",
                "http://media.example.com/fileSequence52-2.ts",
                "http://media.example.com/fileSequence52-3.ts",
            ],
        )
        self.assertEqual([c["duration"] for c in data["segments"]], [15, 15, 15])

    def test_should_load_playlist_with_iv_from_string(self):
        data = m3u8.parse(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV)
        self.assertEqual(data["keys"][0]["uri"], "/hls-key/key.bin")
        self.assertEqual(data["keys"][0]["method"], "AES-128")
        self.assertEqual(data["keys"][0]["iv"], "0X10ef8f758ca555115584bb5b3c687f52")

    def test_should_add_key_attribute_to_segment_from_playlist(self):
        data = m3u8.parse(
            playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV_WITH_MULTIPLE_KEYS
        )
        first_segment_key = data["segments"][0]["key"]
        self.assertEqual(first_segment_key["uri"], "/hls-key/key.bin")
        self.assertEqual(first_segment_key["method"], "AES-128")
        self.assertEqual(first_segment_key["iv"], "0X10ef8f758ca555115584bb5b3c687f52")
        last_segment_key = data["segments"][-1]["key"]
        self.assertEqual(last_segment_key["uri"], "/hls-key/key2.bin")
        self.assertEqual(last_segment_key["method"], "AES-128")
        self.assertEqual(last_segment_key["iv"], "0Xcafe8f758ca555115584bb5b3c687f52")

    def test_should_add_non_key_for_multiple_keys_unencrypted_and_encrypted(self):
        data = m3u8.parse(
            playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED
        )
        # First two segments have no Key, so it's not in the dictionary
        self.assertNotIn("key", data["segments"][0])
        self.assertNotIn("key", data["segments"][1])
        third_segment_key = data["segments"][2]["key"]
        self.assertEqual(third_segment_key["uri"], "/hls-key/key.bin")
        self.assertEqual(third_segment_key["method"], "AES-128")
        self.assertEqual(third_segment_key["iv"], "0X10ef8f758ca555115584bb5b3c687f52")
        last_segment_key = data["segments"][-1]["key"]
        self.assertEqual(last_segment_key["uri"], "/hls-key/key2.bin")
        self.assertEqual(last_segment_key["method"], "AES-128")
        self.assertEqual(last_segment_key["iv"], "0Xcafe8f758ca555115584bb5b3c687f52")

    def test_should_handle_key_method_none_and_no_uri_attr(self):
        data = m3u8.parse(
            playlists.PLAYLIST_WITH_MULTIPLE_KEYS_UNENCRYPTED_AND_ENCRYPTED_NONE_AND_NO_URI_ATTR
        )
        self.assertNotIn("key", data["segments"][0])
        self.assertNotIn("key", data["segments"][1])
        third_segment_key = data["segments"][2]["key"]
        self.assertEqual(third_segment_key["uri"], "/hls-key/key.bin")
        self.assertEqual(third_segment_key["method"], "AES-128")
        self.assertEqual(third_segment_key["iv"], "0X10ef8f758ca555115584bb5b3c687f52")
        self.assertEqual(data["segments"][6]["key"]["method"], "NONE")

    def test_should_parse_playlist_with_session_encrypted_segments_from_string(self):
        data = m3u8.parse(playlists.PLAYLIST_WITH_SESSION_ENCRYPTED_SEGMENTS)
        self.assertEqual(data["media_sequence"], 7794)
        self.assertEqual(data["targetduration"], 15)
        self.assertEqual(data["session_keys"][0]["method"], "AES-128")
        self.assertEqual(
            data["session_keys"][0]["uri"], "https://priv.example.com/key.php?r=52"
        )
        self.assertEqual(
            [c["uri"] for c in data["segments"]],
            [
                "http://media.example.com/fileSequence52-1.ts",
                "http://media.example.com/fileSequence52-2.ts",
                "http://media.example.com/fileSequence52-3.ts",
            ],
        )
        self.assertEqual([c["duration"] for c in data["segments"]], [15, 15, 15])

    def test_should_load_playlist_with_session_iv_from_string(self):
        data = m3u8.parse(playlists.PLAYLIST_WITH_SESSION_ENCRYPTED_SEGMENTS_AND_IV)
        self.assertEqual(data["session_keys"][0]["uri"], "/hls-key/key.bin")
        self.assertEqual(data["session_keys"][0]["method"], "AES-128")
        self.assertEqual(
            data["session_keys"][0]["iv"], "0X10ef8f758ca555115584bb5b3c687f52"
        )

    def test_should_parse_quoted_title_from_playlist(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_WITH_QUOTED_TITLE)
        self.assertEqual(len(data["segments"]), 1)
        self.assertEqual(data["segments"][0]["duration"], 5220)
        self.assertEqual(data["segments"][0]["title"], '"A sample title"')
        self.assertEqual(
            data["segments"][0]["uri"], "http://media.example.com/entire.ts"
        )

    def test_should_parse_unquoted_title_from_playlist(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_WITH_UNQUOTED_TITLE)
        self.assertEqual(len(data["segments"]), 1)
        self.assertEqual(data["segments"][0]["duration"], 5220)
        self.assertEqual(data["segments"][0]["title"], "A sample unquoted title")
        self.assertEqual(
            data["segments"][0]["uri"], "http://media.example.com/entire.ts"
        )

    def test_should_parse_variant_playlist(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST)
        playlists_list = list(data["playlists"])

        self.assertIs(data["is_variant"], True)
        self.assertIsNone(data["media_sequence"])
        self.assertEqual(len(playlists_list), 4)

        self.assertEqual(playlists_list[0]["uri"], "http://example.com/low.m3u8")
        self.assertEqual(playlists_list[0]["stream_info"]["program_id"], 1)
        self.assertEqual(playlists_list[0]["stream_info"]["bandwidth"], 1280000)

        self.assertEqual(
            playlists_list[-1]["uri"], "http://example.com/audio-only.m3u8"
        )
        self.assertEqual(playlists_list[-1]["stream_info"]["program_id"], 1)
        self.assertEqual(playlists_list[-1]["stream_info"]["bandwidth"], 65000)
        self.assertEqual(
            playlists_list[-1]["stream_info"]["codecs"], "mp4a.40.5,avc1.42801e"
        )

    def test_should_parse_variant_playlist_with_cc_subtitles_and_audio(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_CC_SUBS_AND_AUDIO)
        playlists_list = list(data["playlists"])

        self.assertIs(data["is_variant"], True)
        self.assertIsNone(data["media_sequence"])
        self.assertEqual(len(playlists_list), 2)

        self.assertEqual(playlists_list[0]["uri"], "http://example.com/with-cc-hi.m3u8")
        self.assertEqual(playlists_list[0]["stream_info"]["program_id"], 1)
        self.assertEqual(playlists_list[0]["stream_info"]["bandwidth"], 7680000)
        self.assertEqual(playlists_list[0]["stream_info"]["closed_captions"], '"cc"')
        self.assertEqual(playlists_list[0]["stream_info"]["subtitles"], "sub")
        self.assertEqual(playlists_list[0]["stream_info"]["audio"], "aud")

        self.assertEqual(
            playlists_list[-1]["uri"], "http://example.com/with-cc-low.m3u8"
        )
        self.assertEqual(playlists_list[-1]["stream_info"]["program_id"], 1)
        self.assertEqual(playlists_list[-1]["stream_info"]["bandwidth"], 65000)
        self.assertEqual(playlists_list[-1]["stream_info"]["closed_captions"], '"cc"')
        self.assertEqual(playlists_list[-1]["stream_info"]["subtitles"], "sub")
        self.assertEqual(playlists_list[-1]["stream_info"]["audio"], "aud")

    def test_should_parse_variant_playlist_with_none_cc_and_audio(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_NONE_CC_AND_AUDIO)
        playlists_list = list(data["playlists"])

        self.assertEqual(playlists_list[0]["stream_info"]["closed_captions"], "NONE")
        self.assertEqual(playlists_list[-1]["stream_info"]["closed_captions"], "NONE")

    def test_should_parse_variant_playlist_with_average_bandwidth(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_AVERAGE_BANDWIDTH)
        playlists_list = list(data["playlists"])
        self.assertEqual(playlists_list[0]["stream_info"]["bandwidth"], 1280000)
        self.assertEqual(playlists_list[0]["stream_info"]["average_bandwidth"], 1252345)
        self.assertEqual(playlists_list[1]["stream_info"]["bandwidth"], 2560000)
        self.assertEqual(playlists_list[1]["stream_info"]["average_bandwidth"], 2466570)
        self.assertEqual(playlists_list[2]["stream_info"]["bandwidth"], 7680000)
        self.assertEqual(playlists_list[2]["stream_info"]["average_bandwidth"], 7560423)
        self.assertEqual(playlists_list[3]["stream_info"]["bandwidth"], 65000)
        self.assertEqual(playlists_list[3]["stream_info"]["average_bandwidth"], 63005)

    def test_should_parse_variant_playlist_with_video_range(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_VIDEO_RANGE)
        playlists_list = list(data["playlists"])
        self.assertEqual(playlists_list[0]["stream_info"]["video_range"], "SDR")
        self.assertEqual(playlists_list[1]["stream_info"]["video_range"], "PQ")

    def test_should_parse_variant_playlist_with_hdcp_level(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_HDCP_LEVEL)
        playlists_list = list(data["playlists"])
        self.assertEqual(playlists_list[0]["stream_info"]["hdcp_level"], "NONE")
        self.assertEqual(playlists_list[1]["stream_info"]["hdcp_level"], "TYPE-0")
        self.assertEqual(playlists_list[2]["stream_info"]["hdcp_level"], "TYPE-1")

    # This is actually not according to specification but as for example Twitch.tv
    # is producing master playlists that have bandwidth as floats (issue 72)
    # this tests that this situation does not break the parser and will just
    # truncate to a decimal-integer according to specification
    def test_should_parse_variant_playlist_with_bandwidth_as_float(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_BANDWIDTH_FLOAT)
        playlists_list = list(data["playlists"])
        self.assertEqual(playlists_list[0]["stream_info"]["bandwidth"], 1280000)
        self.assertEqual(playlists_list[1]["stream_info"]["bandwidth"], 2560000)
        self.assertEqual(playlists_list[2]["stream_info"]["bandwidth"], 7680000)
        self.assertEqual(playlists_list[3]["stream_info"]["bandwidth"], 65000)

    def test_should_parse_variant_playlist_with_iframe_playlists(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_IFRAME_PLAYLISTS)
        iframe_playlists = list(data["iframe_playlists"])

        self.assertIs(data["is_variant"], True)

        self.assertEqual(len(iframe_playlists), 4)

        self.assertEqual(iframe_playlists[0]["iframe_stream_info"]["program_id"], 1)
        self.assertEqual(iframe_playlists[0]["iframe_stream_info"]["bandwidth"], 151288)
        self.assertEqual(
            iframe_playlists[0]["iframe_stream_info"]["resolution"], "624x352"
        )
        self.assertEqual(
            iframe_playlists[0]["iframe_stream_info"]["codecs"], "avc1.4d001f"
        )
        self.assertEqual(iframe_playlists[0]["uri"], "video-800k-iframes.m3u8")

        self.assertEqual(iframe_playlists[-1]["iframe_stream_info"]["bandwidth"], 38775)
        self.assertEqual(
            iframe_playlists[-1]["iframe_stream_info"]["codecs"], "avc1.4d001f"
        )
        self.assertEqual(iframe_playlists[-1]["uri"], "video-150k-iframes.m3u8")

    def test_should_parse_variant_playlist_with_alt_iframe_playlists_layout(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_ALT_IFRAME_PLAYLISTS_LAYOUT)
        iframe_playlists = list(data["iframe_playlists"])

        self.assertIs(data["is_variant"], True)

        self.assertEqual(len(iframe_playlists), 4)

        self.assertEqual(iframe_playlists[0]["iframe_stream_info"]["program_id"], 1)
        self.assertEqual(iframe_playlists[0]["iframe_stream_info"]["bandwidth"], 151288)
        self.assertEqual(
            iframe_playlists[0]["iframe_stream_info"]["resolution"], "624x352"
        )
        self.assertEqual(
            iframe_playlists[0]["iframe_stream_info"]["codecs"], "avc1.4d001f"
        )
        self.assertEqual(iframe_playlists[0]["uri"], "video-800k-iframes.m3u8")

        self.assertEqual(iframe_playlists[-1]["iframe_stream_info"]["bandwidth"], 38775)
        self.assertEqual(
            iframe_playlists[-1]["iframe_stream_info"]["codecs"], "avc1.4d001f"
        )
        self.assertEqual(iframe_playlists[-1]["uri"], "video-150k-iframes.m3u8")

    def test_should_parse_iframe_playlist(self):
        data = m3u8.parse(playlists.IFRAME_PLAYLIST)

        self.assertIs(data["is_i_frames_only"], True)
        self.assertEqual(data["segments"][0]["duration"], 4.12)
        self.assertEqual(data["segments"][0]["byterange"], "9400@376")
        self.assertEqual(data["segments"][0]["uri"], "segment1.ts")

    def test_should_parse_variant_playlist_with_image_playlists(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_IMAGE_PLAYLISTS)
        image_playlists = list(data["image_playlists"])

        self.assertIs(data["is_variant"], True)
        self.assertEqual(len(image_playlists), 2)
        self.assertEqual(
            image_playlists[0]["image_stream_info"]["resolution"], "320x180"
        )
        self.assertEqual(image_playlists[0]["image_stream_info"]["codecs"], "jpeg")
        self.assertEqual(image_playlists[0]["uri"], "5x2_320x180/320x180-5x2.m3u8")
        self.assertEqual(
            image_playlists[1]["image_stream_info"]["resolution"], "640x360"
        )
        self.assertEqual(image_playlists[1]["image_stream_info"]["codecs"], "jpeg")
        self.assertEqual(image_playlists[1]["uri"], "5x2_640x360/640x360-5x2.m3u8")

    def test_should_parse_vod_image_playlist(self):
        data = m3u8.parse(playlists.VOD_IMAGE_PLAYLIST)

        self.assertIs(data["is_images_only"], True)
        self.assertEqual(len(data["tiles"]), 8)
        self.assertEqual(data["segments"][0]["uri"], "preroll-ad-1.jpg")
        self.assertEqual(data["tiles"][0]["resolution"], "640x360")
        self.assertEqual(data["tiles"][0]["layout"], "5x2")
        self.assertEqual(data["tiles"][0]["duration"], 6.006)
        self.assertNotIn("byterange", data["tiles"][0])

    def test_should_parse_vod_image_playlist2(self):
        data = m3u8.parse(playlists.VOD_IMAGE_PLAYLIST2)

        self.assertIs(data["is_images_only"], True)
        self.assertEqual(data["tiles"][0]["resolution"], "640x360")
        self.assertEqual(data["tiles"][0]["layout"], "4x3")
        self.assertEqual(data["tiles"][0]["duration"], 2.002)
        self.assertEqual(len(data["tiles"]), 6)
        self.assertEqual(data["segments"][0]["uri"], "promo_1.jpg")

    def test_should_parse_live_image_playlist(self):
        data = m3u8.parse(playlists.LIVE_IMAGE_PLAYLIST)

        self.assertIs(data["is_images_only"], True)
        self.assertEqual(len(data["segments"]), 10)
        self.assertEqual(data["segments"][0]["uri"], "content-123.jpg")
        self.assertEqual(data["segments"][1]["uri"], "content-124.jpg")
        self.assertEqual(data["segments"][2]["uri"], "content-125.jpg")
        self.assertEqual(data["segments"][3]["uri"], "missing-midroll.jpg")
        self.assertEqual(data["segments"][4]["uri"], "missing-midroll.jpg")
        self.assertEqual(data["segments"][5]["uri"], "missing-midroll.jpg")
        self.assertEqual(data["segments"][6]["uri"], "content-128.jpg")
        self.assertEqual(data["segments"][7]["uri"], "content-129.jpg")
        self.assertEqual(data["segments"][8]["uri"], "content-130.jpg")
        self.assertEqual(data["segments"][9]["uri"], "content-131.jpg")

    def test_should_parse_playlist_using_byteranges(self):
        data = m3u8.parse(playlists.PLAYLIST_USING_BYTERANGES)

        self.assertIs(data["is_i_frames_only"], False)
        self.assertEqual(data["segments"][0]["duration"], 10)
        self.assertEqual(data["segments"][0]["byterange"], "76242@0")
        self.assertEqual(data["segments"][0]["uri"], "segment.ts")

    def test_should_parse_endlist_playlist(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST)
        self.assertIs(data["is_endlist"], True)

        data = m3u8.parse(playlists.SLIDING_WINDOW_PLAYLIST)
        self.assertIs(data["is_endlist"], False)

    def test_should_parse_ALLOW_CACHE(self):
        data = m3u8.parse(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV)
        self.assertEqual(data["allow_cache"], "no")

    def test_should_parse_VERSION(self):
        data = m3u8.parse(playlists.PLAYLIST_WITH_ENCRYPTED_SEGMENTS_AND_IV)
        self.assertEqual(data["version"], 2)

    def test_should_parse_program_date_time_from_playlist(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_WITH_PROGRAM_DATE_TIME)
        self.assertEqual(
            data["program_date_time"], cast_date_time("2014-08-13T13:36:33+00:00")
        )

    def test_should_parse_scte35_from_playlist(self):
        data = m3u8.parse(playlists.CUE_OUT_ELEMENTAL_PLAYLIST)

        # cue_out should be maintained from [EXT-X-CUE-OUT, EXT-X-CUE-IN)
        actual_cue_status = [s["cue_out"] for s in data["segments"]]
        expected_cue_status = [
            False,
            False,
            False,
            True,  # EXT-X-CUE-OUT
            True,
            True,
            True,
            True,
            True,
            False,  # EXT-X-CUE-IN
            False,
        ]
        self.assertEqual(actual_cue_status, expected_cue_status)

        # scte35 should be maintained from [EXT-X-CUE-OUT, EXT-X-CUE-IN]
        cue = "/DAlAAAAAAAAAP/wFAUAAAABf+//wpiQkv4ARKogAAEBAQAAQ6sodg=="
        actual_scte35 = [s["scte35"] for s in data["segments"]]
        expected_scte35 = [None, None, None, cue, cue, cue, cue, cue, cue, cue, None]
        self.assertEqual(actual_scte35, expected_scte35)

        # oatcls_scte35 should be maintained until it changes
        oat = "/DA2AAAAAAAAAP/wBQb//vOoWQAgAh5DVUVJbpFcxn+/DA9XVVJMRDAwMDAwMzU4NTkQAAA5VeXi"
        actual_oatcls_scte35 = [s["oatcls_scte35"] for s in data["segments"]]
        expected_oatcls_scte35 = [
            None,
            None,
            None,
            cue,
            cue,
            cue,
            cue,
            cue,
            oat,
            oat,
            None,
        ]
        self.assertEqual(actual_oatcls_scte35, expected_oatcls_scte35)

        # durations should be maintained from  from [EXT-X-CUE-OUT, EXT-X-CUE-IN]
        actual_scte35_duration = [s["scte35_duration"] for s in data["segments"]]
        expected_scte35_duration = [
            None,
            None,
            None,
            "50.000",
            "50",
            "50",
            "50",
            "50",
            "50",
            "50",
            None,
        ]
        self.assertEqual(actual_scte35_duration, expected_scte35_duration)

    def test_should_parse_envivio_cue_playlist(self):
        data = m3u8.parse(playlists.CUE_OUT_ENVIVIO_PLAYLIST)
        self.assertTrue(data["segments"][3]["scte35"])
        self.assertTrue(data["segments"][3]["cue_out"])
        self.assertEqual(
            data["segments"][3]["scte35"],
            "/DAlAAAENOOQAP/wFAUBAABrf+//N25XDf4B9p/gAAEBAQAAxKni9A==",
        )
        self.assertEqual(data["segments"][3]["scte35_duration"], "366")
        self.assertTrue(data["segments"][4]["cue_out"])
        self.assertEqual(
            data["segments"][4]["scte35"],
            "/DAlAAAENOOQAP/wFAUBAABrf+//N25XDf4B9p/gAAEBAQAAxKni9A==",
        )
        self.assertEqual(
            data["segments"][5]["scte35"],
            "/DAlAAAENOOQAP/wFAUBAABrf+//N25XDf4B9p/gAAEBAQAAxKni9A==",
        )

    def test_should_parse_no_duration_cue_playlist(self):
        data = m3u8.parse(playlists.CUE_OUT_NO_DURATION_PLAYLIST)
        self.assertTrue(data["segments"][0]["cue_out_start"])
        self.assertTrue(data["segments"][2]["cue_in"])

    def test_parse_simple_playlist_messy(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_MESSY)
        self.assertEqual(data["targetduration"], 5220)
        self.assertEqual(data["media_sequence"], 0)
        self.assertEqual(
            [c["uri"] for c in data["segments"]],
            ["http://media.example.com/entire.ts"],
        )
        self.assertEqual([c["duration"] for c in data["segments"]], [5220])

    def test_parse_simple_playlist_messy_strict(self):
        with self.assertRaises(ParseError) as cm:
            m3u8.parse(playlists.SIMPLE_PLAYLIST_MESSY, strict=True)
        self.assertEqual(str(cm.exception), "Syntax error in manifest on line 5: JUNK")

    def test_commaless_extinf(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_COMMALESS_EXTINF)
        self.assertEqual(data["targetduration"], 5220)
        self.assertEqual(data["media_sequence"], 0)
        self.assertEqual(
            [c["uri"] for c in data["segments"]],
            ["http://media.example.com/entire.ts"],
        )
        self.assertEqual([c["duration"] for c in data["segments"]], [5220])

    def test_commaless_extinf_strict(self):
        with self.assertRaises(ParseError) as cm:
            m3u8.parse(playlists.SIMPLE_PLAYLIST_COMMALESS_EXTINF, strict=True)
        self.assertEqual(
            str(cm.exception), "Syntax error in manifest on line 3: #EXTINF:5220"
        )

    def test_should_parse_segment_map_uri(self):
        data = m3u8.parse(playlists.MAP_URI_PLAYLIST)
        self.assertEqual(data["segment_map"][0]["uri"], "fileSequence0.mp4")

    def test_should_parse_segment_map_uri_with_byterange(self):
        data = m3u8.parse(playlists.MAP_URI_PLAYLIST_WITH_BYTERANGE)
        self.assertEqual(data["segment_map"][0]["uri"], "main.mp4")

    def test_should_parse_multiple_map_attributes(self):
        data = m3u8.parse(playlists.MULTIPLE_MAP_URI_PLAYLIST)

        self.assertEqual(data["segments"][0]["init_section"]["uri"], "init1.mp4")
        self.assertEqual(data["segments"][1]["init_section"]["uri"], "init1.mp4")
        self.assertEqual(data["segments"][2]["init_section"]["uri"], "init3.mp4")

    def test_should_parse_empty_map_tag(self):
        data = m3u8.parse(playlists.EMPTY_MAP_TAG_PLAYLIST)

        # Empty MAP tag should result in an empty dict in segment_map
        self.assertEqual(data["segment_map"], [{}])
        # But segments should not have init_section set (empty dict is falsy)
        self.assertNotIn("init_section", data["segments"][0])
        self.assertNotIn("init_section", data["segments"][1])

    def test_should_parse_empty_uri_with_base_path(self):
        data = m3u8.M3U8(
            playlists.MEDIA_WITHOUT_URI_PLAYLIST,
            base_path="base_path",
            base_uri="base_uri",
        )
        media = data.media[0]
        self.assertIsNone(media.uri)
        self.assertIsNone(media.base_path)
        self.assertEqual(media.base_uri, "base_uri/")

    def test_should_parse_audio_channels(self):
        data = m3u8.M3U8(
            playlists.MEDIA_WITHOUT_URI_PLAYLIST,
            base_path="base_path",
            base_uri="base_uri",
        )
        media = data.media[0]
        self.assertEqual(media.channels, "2")

    def test_should_parse_start_with_negative_time_offset(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_WITH_START_NEGATIVE_OFFSET)
        self.assertEqual(data["start"]["time_offset"], -2.0)
        self.assertFalse(hasattr(data["start"], "precise"))

    def test_should_parse_start_with_precise(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_WITH_START_PRECISE)
        self.assertEqual(data["start"]["time_offset"], 10.5)
        self.assertEqual(data["start"]["precise"], "YES")

    def test_should_parse_session_data(self):
        data = m3u8.parse(playlists.SESSION_DATA_PLAYLIST)
        self.assertEqual(data["session_data"][0]["data_id"], "com.example.value")
        self.assertEqual(data["session_data"][0]["value"], "example")
        self.assertEqual(data["session_data"][0]["language"], "en")

    def test_should_parse_multiple_session_data(self):
        data = m3u8.parse(playlists.MULTIPLE_SESSION_DATA_PLAYLIST)

        self.assertEqual(len(data["session_data"]), 4)

        self.assertEqual(data["session_data"][0]["data_id"], "com.example.value")
        self.assertEqual(data["session_data"][0]["value"], "example")
        self.assertEqual(data["session_data"][0]["language"], "en")

        self.assertEqual(data["session_data"][1]["data_id"], "com.example.value")
        self.assertEqual(data["session_data"][1]["value"], "example")
        self.assertEqual(data["session_data"][1]["language"], "ru")

        self.assertEqual(data["session_data"][2]["data_id"], "com.example.value")
        self.assertEqual(data["session_data"][2]["value"], "example")
        self.assertEqual(data["session_data"][2]["language"], "de")

        self.assertEqual(data["session_data"][3]["data_id"], "com.example.title")
        self.assertEqual(data["session_data"][3]["uri"], "title.json")

    def test_simple_playlist_with_discontinuity_sequence(self):
        data = m3u8.parse(playlists.SIMPLE_PLAYLIST_WITH_DISCONTINUITY_SEQUENCE)
        self.assertEqual(data["discontinuity_sequence"], 123)

    def test_simple_playlist_with_custom_tags(self):
        def get_movie(line, lineno, data, segment):
            if line.startswith("#EXT-X-MOVIE"):
                custom_tag = line.split(":")
                if len(custom_tag) == 2:
                    data["movie"] = custom_tag[1].strip()
                    return True

        data = m3u8.parse(
            playlists.SIMPLE_PLAYLIST_WITH_CUSTOM_TAGS,
            strict=False,
            custom_tags_parser=get_movie,
        )
        self.assertEqual(data["movie"], "million dollar baby")
        self.assertEqual(data["targetduration"], 5220)
        self.assertEqual(data["media_sequence"], 0)
        self.assertEqual(
            [c["uri"] for c in data["segments"]],
            ["http://media.example.com/entire.ts"],
        )
        self.assertEqual([c["duration"] for c in data["segments"]], [5220])

    def test_iptv_playlist_with_custom_tags(self):
        def parse_iptv_attributes(line, lineno, data, state):
            # Customize parsing #EXTINF
            if line.startswith("#EXTINF"):
                chunks = line.replace("#EXTINF" + ":", "").split(",", 1)
                if len(chunks) == 2:
                    duration_and_props, title = chunks
                elif len(chunks) == 1:
                    duration_and_props = chunks[0]
                    title = ""

                additional_props = {}
                chunks = duration_and_props.strip().split(" ", 1)
                if len(chunks) == 2:
                    duration, raw_props = chunks
                    matched_props = re.finditer(r'([\w\-]+)="([^"]*)"', raw_props)
                    for match in matched_props:
                        additional_props[match.group(1)] = match.group(2)
                else:
                    duration = duration_and_props

                if "segment" not in state:
                    state["segment"] = {}
                state["segment"]["duration"] = float(duration)
                state["segment"]["title"] = title

                save_segment_custom_value(state, "extinf_props", additional_props)

                state["expect_segment"] = True
                return True

            # Parse #EXTGRP
            if line.startswith("#EXTGRP"):
                _, value = _parse_simple_parameter_raw_value(line, str)
                save_segment_custom_value(state, "extgrp", value)
                state["expect_segment"] = True
                return True

            # Parse #EXTVLCOPT
            if line.startswith("#EXTVLCOPT"):
                _, value = _parse_simple_parameter_raw_value(line, str)

                existing_opts = get_segment_custom_value(state, "vlcopt", [])
                existing_opts.append(value)
                save_segment_custom_value(state, "vlcopt", existing_opts)

                state["expect_segment"] = True
                return True

        data = m3u8.parse(
            playlists.IPTV_PLAYLIST_WITH_CUSTOM_TAGS,
            strict=False,
            custom_tags_parser=parse_iptv_attributes,
        )

        self.assertEqual([c["title"] for c in data["segments"]], ["Channel1"])
        self.assertEqual(
            data["segments"][0]["uri"],
            "http://str00.iptv.domain/7331/mpegts?token=longtokenhere",
        )
        self.assertEqual(
            data["segments"][0]["custom_parser_values"]["extinf_props"]["tvg-id"],
            "channel1",
        )
        self.assertEqual(
            data["segments"][0]["custom_parser_values"]["extinf_props"]["group-title"],
            "Group1",
        )
        self.assertEqual(
            data["segments"][0]["custom_parser_values"]["extinf_props"]["catchup-days"],
            "7",
        )
        self.assertEqual(
            data["segments"][0]["custom_parser_values"]["extinf_props"]["catchup-type"],
            "flussonic",
        )
        self.assertEqual(
            data["segments"][0]["custom_parser_values"]["extgrp"], "ExtGroup1"
        )
        self.assertEqual(
            data["segments"][0]["custom_parser_values"]["vlcopt"],
            [
                "video-filter=invert",
                "param2=value2",
            ],
        )

    def test_tag_after_extinf(self):
        parsed_playlist = m3u8.loads(playlists.IPTV_PLAYLIST_WITH_EARLY_EXTINF)
        actual = parsed_playlist.segments[0].uri
        expected = "http://str00.iptv.domain/7331/mpegts?token=longtokenhere"
        self.assertEqual(actual, expected)

    def test_master_playlist_with_frame_rate(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_FRAME_RATE)
        playlists_list = list(data["playlists"])
        self.assertEqual(playlists_list[0]["stream_info"]["frame_rate"], 25)
        self.assertEqual(playlists_list[1]["stream_info"]["frame_rate"], 50)
        self.assertEqual(playlists_list[2]["stream_info"]["frame_rate"], 60)
        self.assertEqual(playlists_list[3]["stream_info"]["frame_rate"], 12.5)

    def test_master_playlist_with_unrounded_frame_rate(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_ROUNDABLE_FRAME_RATE)
        playlists_list = list(data["playlists"])
        self.assertEqual(playlists_list[0]["stream_info"]["frame_rate"], 12.54321)

    def test_low_latency_playlist(self):
        data = m3u8.parse(playlists.LOW_LATENCY_DELTA_UPDATE_PLAYLIST)
        self.assertEqual(data["server_control"]["can_block_reload"], "YES")
        self.assertEqual(data["server_control"]["can_skip_until"], 12.0)
        self.assertEqual(data["server_control"]["part_hold_back"], 1.0)
        self.assertEqual(data["part_inf"]["part_target"], 0.33334)
        self.assertEqual(data["skip"]["skipped_segments"], 3)
        self.assertEqual(len(data["segments"][2]["parts"]), 12)
        self.assertEqual(data["segments"][2]["parts"][0]["duration"], 0.33334)
        self.assertEqual(data["segments"][2]["parts"][0]["uri"], "filePart271.0.ts")
        self.assertEqual(len(data["rendition_reports"]), 2)
        self.assertEqual(data["rendition_reports"][0]["uri"], "../1M/waitForMSN.php")
        self.assertEqual(data["rendition_reports"][0]["last_msn"], 273)
        self.assertEqual(data["rendition_reports"][0]["last_part"], 3)

    def test_low_latency_with_preload_and_byteranges_playlist(self):
        data = m3u8.parse(playlists.LOW_LATENCY_WITH_PRELOAD_AND_BYTERANGES_PLAYLIST)
        self.assertEqual(data["segments"][1]["parts"][2]["byterange"], "18000@43000")
        self.assertEqual(data["preload_hint"]["type"], "PART")
        self.assertEqual(data["preload_hint"]["uri"], "fs271.mp4")
        self.assertEqual(data["preload_hint"]["byterange_start"], 61000)
        self.assertEqual(data["preload_hint"]["byterange_length"], 20000)

    def test_negative_media_sequence(self):
        data = m3u8.parse(playlists.PLAYLIST_WITH_NEGATIVE_MEDIA_SEQUENCE)
        self.assertEqual(data["media_sequence"], -2680)

    def test_daterange_simple(self):
        data = m3u8.parse(playlists.DATERANGE_SIMPLE_PLAYLIST)

        self.assertEqual(data["segments"][0]["dateranges"][0]["id"], "ad3")
        self.assertEqual(
            data["segments"][0]["dateranges"][0]["start_date"], "2016-06-13T11:15:00Z"
        )
        self.assertEqual(data["segments"][0]["dateranges"][0]["duration"], 20)
        self.assertEqual(data["segments"][0]["dateranges"][0]["x_ad_id"], '"1234"')
        self.assertEqual(
            data["segments"][0]["dateranges"][0]["x_ad_url"],
            '"http://ads.example.com/beacon3"',
        )

    def test_date_range_with_scte_out_and_in(self):
        data = m3u8.parse(playlists.DATERANGE_SCTE35_OUT_AND_IN_PLAYLIST)

        self.assertEqual(data["segments"][0]["dateranges"][0]["id"], "splice-6FFFFFF0")
        self.assertEqual(
            data["segments"][0]["dateranges"][0]["planned_duration"], 59.993
        )
        self.assertEqual(
            data["segments"][0]["dateranges"][0]["scte35_out"],
            "0xFC002F0000000000FF000014056FFFFFF000E011622DCAFF000052636200000000000A0008029896F50000008700000000",
        )

        self.assertEqual(data["segments"][6]["dateranges"][0]["id"], "splice-6FFFFFF0")
        self.assertEqual(data["segments"][6]["dateranges"][0]["duration"], 59.993)
        self.assertEqual(
            data["segments"][6]["dateranges"][0]["scte35_in"],
            "0xFC002A0000000000FF00000F056FFFFFF000401162802E6100000000000A0008029896F50000008700000000",
        )

    def test_date_range_in_parts(self):
        data = m3u8.parse(playlists.DATERANGE_IN_PART_PLAYLIST)

        self.assertEqual(
            data["segments"][0]["parts"][2]["dateranges"][0]["id"], "test_id"
        )
        self.assertEqual(
            data["segments"][0]["parts"][2]["dateranges"][0]["start_date"],
            "2020-03-10T07:48:02Z",
        )
        self.assertEqual(
            data["segments"][0]["parts"][2]["dateranges"][0]["class"], "test_class"
        )
        self.assertEqual(
            data["segments"][0]["parts"][2]["dateranges"][0]["end_on_next"], "YES"
        )

    def test_gap(self):
        data = m3u8.parse(playlists.GAP_PLAYLIST)

        self.assertIsNone(data["segments"][0]["gap_tag"])
        self.assertIs(data["segments"][1]["gap_tag"], True)
        self.assertIs(data["segments"][2]["gap_tag"], True)
        self.assertIsNone(data["segments"][3]["gap_tag"])

    def test_gap_in_parts(self):
        data = m3u8.parse(playlists.GAP_IN_PARTS_PLAYLIST)

        self.assertIsNone(data["segments"][0]["parts"][0]["gap_tag"])
        self.assertIsNone(data["segments"][0]["parts"][0].get("gap", None))
        self.assertIsNone(data["segments"][0]["parts"][1]["gap_tag"])
        self.assertEqual(data["segments"][0]["parts"][1]["gap"], "YES")
        self.assertIs(data["segments"][0]["parts"][2]["gap_tag"], True)
        self.assertIsNone(data["segments"][0]["parts"][2].get("gap", None))

    def test_blackout_simple(self):
        data = m3u8.parse(playlists.BLACKOUT_SIMPLE_PLAYLIST)

        self.assertIsNone(data["segments"][0]["blackout"])
        self.assertIs(data["segments"][1]["blackout"], True)
        self.assertIsNone(data["segments"][2]["blackout"])

    def test_blackout_with_params(self):
        data = m3u8.parse(playlists.BLACKOUT_WITH_PARAMS_PLAYLIST)

        self.assertIsNone(data["segments"][0]["blackout"])
        self.assertEqual(data["segments"][1]["blackout"], "ID=12345,DURATION=30.0")
        self.assertIsNone(data["segments"][2]["blackout"])

    def test_should_parse_variant_playlist_with_iframe_with_average_bandwidth(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_IFRAME_AVERAGE_BANDWIDTH)
        iframe_playlists = list(data["iframe_playlists"])

        self.assertIs(data["is_variant"], True)

        self.assertEqual(len(iframe_playlists), 4)

        self.assertEqual(iframe_playlists[0]["iframe_stream_info"]["bandwidth"], 151288)
        # Check for absence of average_bandwidth if not given in the playlist
        self.assertNotIn("average_bandwidth", iframe_playlists[0]["iframe_stream_info"])
        self.assertEqual(
            iframe_playlists[0]["iframe_stream_info"]["resolution"], "624x352"
        )
        self.assertEqual(
            iframe_playlists[0]["iframe_stream_info"]["codecs"], "avc1.4d001f"
        )
        self.assertEqual(iframe_playlists[0]["uri"], "video-800k-iframes.m3u8")

        self.assertEqual(iframe_playlists[-1]["iframe_stream_info"]["bandwidth"], 38775)
        self.assertEqual(
            iframe_playlists[-1]["iframe_stream_info"]["codecs"], "avc1.4d001f"
        )
        self.assertEqual(iframe_playlists[-1]["uri"], "video-150k-iframes.m3u8")
        self.assertEqual(
            iframe_playlists[1]["iframe_stream_info"]["average_bandwidth"], 155000
        )
        self.assertEqual(
            iframe_playlists[2]["iframe_stream_info"]["average_bandwidth"], 65000
        )
        self.assertEqual(
            iframe_playlists[3]["iframe_stream_info"]["average_bandwidth"], 30000
        )

    def test_should_parse_variant_playlist_with_iframe_with_video_range(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_IFRAME_VIDEO_RANGE)
        iframe_playlists = list(data["iframe_playlists"])

        self.assertIs(data["is_variant"], True)

        self.assertEqual(len(iframe_playlists), 4)

        self.assertEqual(
            iframe_playlists[0]["uri"], "http://example.com/sdr-iframes.m3u8"
        )
        self.assertEqual(
            iframe_playlists[0]["iframe_stream_info"]["video_range"], "SDR"
        )
        self.assertEqual(
            iframe_playlists[1]["uri"], "http://example.com/hdr-pq-iframes.m3u8"
        )
        self.assertEqual(iframe_playlists[1]["iframe_stream_info"]["video_range"], "PQ")
        self.assertEqual(
            iframe_playlists[2]["uri"], "http://example.com/hdr-hlg-iframes.m3u8"
        )
        self.assertEqual(
            iframe_playlists[2]["iframe_stream_info"]["video_range"], "HLG"
        )
        self.assertEqual(
            iframe_playlists[3]["uri"], "http://example.com/unknown-iframes.m3u8"
        )
        self.assertNotIn("video_range", iframe_playlists[3]["iframe_stream_info"])

    def test_should_parse_variant_playlist_with_iframe_with_hdcp_level(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_IFRAME_HDCP_LEVEL)
        iframe_playlists = list(data["iframe_playlists"])

        self.assertIs(data["is_variant"], True)

        self.assertEqual(len(iframe_playlists), 4)

        self.assertEqual(
            iframe_playlists[0]["uri"], "http://example.com/none-iframes.m3u8"
        )
        self.assertEqual(
            iframe_playlists[0]["iframe_stream_info"]["hdcp_level"], "NONE"
        )
        self.assertEqual(
            iframe_playlists[1]["uri"], "http://example.com/type0-iframes.m3u8"
        )
        self.assertEqual(
            iframe_playlists[1]["iframe_stream_info"]["hdcp_level"], "TYPE-0"
        )
        self.assertEqual(
            iframe_playlists[2]["uri"], "http://example.com/type1-iframes.m3u8"
        )
        self.assertEqual(
            iframe_playlists[2]["iframe_stream_info"]["hdcp_level"], "TYPE-1"
        )
        self.assertEqual(
            iframe_playlists[3]["uri"], "http://example.com/unknown-iframes.m3u8"
        )
        self.assertNotIn("hdcp_level", iframe_playlists[3]["iframe_stream_info"])

    def test_delta_playlist_daterange_skipping(self):
        data = m3u8.parse(playlists.DELTA_UPDATE_SKIP_DATERANGES_PLAYLIST)
        self.assertEqual(data["skip"]["recently_removed_dateranges"], "1")
        self.assertEqual(data["server_control"]["can_skip_dateranges"], "YES")

    def test_bitrate(self):
        data = m3u8.parse(playlists.BITRATE_PLAYLIST)
        self.assertEqual(data["segments"][0]["bitrate"], 1674)
        self.assertEqual(data["segments"][1]["bitrate"], 1625)

    def test_content_steering(self):
        data = m3u8.parse(playlists.CONTENT_STEERING_PLAYLIST)
        self.assertEqual(
            data["content_steering"]["server_uri"], "/steering?video=00012"
        )
        self.assertEqual(data["content_steering"]["pathway_id"], "CDN-A")
        self.assertEqual(data["playlists"][0]["stream_info"]["pathway_id"], "CDN-A")
        self.assertEqual(data["playlists"][1]["stream_info"]["pathway_id"], "CDN-A")
        self.assertEqual(data["playlists"][2]["stream_info"]["pathway_id"], "CDN-B")
        self.assertEqual(data["playlists"][3]["stream_info"]["pathway_id"], "CDN-B")

    def test_cue_in_pops_scte35_data_and_duration(self):
        data = m3u8.parse(playlists.CUE_OUT_ELEMENTAL_PLAYLIST)
        self.assertIs(data["segments"][9]["cue_in"], True)
        self.assertEqual(
            data["segments"][9]["scte35"],
            "/DAlAAAAAAAAAP/wFAUAAAABf+//wpiQkv4ARKogAAEBAQAAQ6sodg==",
        )
        self.assertEqual(data["segments"][9]["scte35_duration"], "50")
        self.assertIs(data["segments"][10]["cue_in"], False)
        self.assertIsNone(data["segments"][10]["scte35"])
        self.assertIsNone(data["segments"][10]["scte35_duration"])

    def test_playlist_with_stable_variant_id(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_STABLE_VARIANT_ID)
        self.assertEqual(
            data["playlists"][0]["stream_info"]["stable_variant_id"],
            "eb9c6e4de930b36d9a67fbd38a30b39f865d98f4a203d2140bbf71fd58ad764e",
        )

    def test_iframe_with_stable_variant_id(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_IFRAME_STABLE_VARIANT_ID)
        self.assertEqual(
            data["iframe_playlists"][0]["iframe_stream_info"]["stable_variant_id"],
            "415901312adff69b967a0644a54f8d00dc14004f36bc8293737e6b4251f60f3f",
        )

    def test_media_with_stable_rendition_id(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_STABLE_RENDITION_ID)
        self.assertEqual(
            data["media"][0]["stable_rendition_id"],
            "a8213e27c12a158ea8660e0fe8bdcac6072ca26d984e7e8603652bc61fdceffa",
        )

    def test_req_video_layout(self):
        data = m3u8.parse(playlists.VARIANT_PLAYLIST_WITH_REQ_VIDEO_LAYOUT)
        self.assertEqual(
            data["playlists"][0]["stream_info"]["req_video_layout"], '"CH-STEREO"'
        )
