# Copyright 2014 Globo.com Player authors. All rights reserved.
# Modifications Copyright (c) 2026 Wurl.
# Use of this source code is governed by a MIT License
# license that can be found in the LICENSE file.

from unittest import TestCase, expectedFailure

import openm3u8 as m3u8
from tests import invalid_versioned_playlists


class StrictValidationsTests(TestCase):
    @expectedFailure
    def test_should_fail_if_first_line_not_EXTM3U(self):
        self.fail()

    @expectedFailure
    def test_should_fail_if_expected_ts_segment_line_is_not_valid(self):
        self.fail()

    @expectedFailure
    def test_should_fail_if_EXT_X_MEDIA_SEQUENCE_is_diffent_from_sequence_number_of_first_uri(
        self,
    ):
        self.fail()

    @expectedFailure
    def test_should_fail_if_more_than_one_EXT_X_MEDIA_SEQUENCE(self):
        self.fail()

    @expectedFailure
    def test_should_fail_if_EXT_X_MEDIA_SEQUENCE_is_not_a_number(self):
        self.fail()

    def test_should_validate_supported_EXT_X_VERSION(self):
        with self.assertRaises(Exception):
            m3u8.parse(invalid_versioned_playlists.M3U8_RULE_IV, strict=True)

    @expectedFailure
    def test_should_fail_if_any_EXTINF_duration_is_greater_than_TARGET_DURATION(self):
        self.fail()

    @expectedFailure
    def test_should_fail_if_TARGET_DURATION_not_found(self):
        self.fail()

    @expectedFailure
    def test_should_fail_if_invalid_m3u8_url_after_EXT_X_STREAM_INF(self):
        self.fail()
