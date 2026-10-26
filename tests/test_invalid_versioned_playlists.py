from unittest import TestCase

import openm3u8 as m3u8
from tests import invalid_versioned_playlists


class InvalidVersionedPlaylistsTests(TestCase):
    def test_should_fail_if_iv_in_EXT_X_KEY_and_version_less_than_2(self):
        with self.assertRaises(Exception) as cm:
            m3u8.parse(invalid_versioned_playlists.M3U8_RULE_IV, strict=True)

        self.assertIn("Change the protocol version to 2 or higher.", str(cm.exception))

    def test_should_fail_if_floating_point_EXTINF_and_version_less_than_3(self):
        with self.assertRaises(Exception) as cm:
            m3u8.parse(
                invalid_versioned_playlists.M3U8_RULE_FLOATING_POINT, strict=True
            )

        self.assertIn("Change the protocol version to 3 or higher.", str(cm.exception))

    def test_should_fail_if_EXT_X_BYTERANGE_or_EXT_X_I_FRAMES_ONLY_and_version_less_than_4(
        self,
    ):
        with self.assertRaises(Exception) as cm:
            m3u8.parse(invalid_versioned_playlists.M3U8_RULE_BYTE_RANGE, strict=True)

        self.assertIn("Change the protocol version to 4 or higher.", str(cm.exception))
