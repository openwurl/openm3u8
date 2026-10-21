# Copyright 2014 Globo.com Player authors. All rights reserved.
# Modifications Copyright (c) 2026 Wurl.
# Use of this source code is governed by a MIT License
# license that can be found in the LICENSE file.

import os
import socket
import urllib.parse
from http.client import HTTPResponse
from unittest import TestCase
from unittest.mock import Mock, patch

import openm3u8 as m3u8
from tests import playlists


class LoaderTests(TestCase):
    def test_loads_should_create_object_from_string(self):
        obj = m3u8.loads(playlists.SIMPLE_PLAYLIST)
        self.assertIsInstance(obj, m3u8.M3U8)
        self.assertEqual(obj.target_duration, 5220)
        self.assertEqual(obj.segments[0].uri, "http://media.example.com/entire.ts")

    def test_load_should_create_object_from_file(self):
        obj = m3u8.load(playlists.SIMPLE_PLAYLIST_FILENAME)
        self.assertIsInstance(obj, m3u8.M3U8)
        self.assertEqual(obj.target_duration, 5220)
        self.assertEqual(obj.segments[0].uri, "http://media.example.com/entire.ts")

    @patch("urllib.request.OpenerDirector.open")
    def test_load_should_create_object_from_uri(self, mock_open):
        mock_response = Mock(spec=HTTPResponse)
        mock_response.read.return_value = playlists.SIMPLE_PLAYLIST.encode("utf-8")
        mock_response.info.return_value = {}
        mock_response.geturl.return_value = playlists.SIMPLE_PLAYLIST_URI
        mock_response.headers = Mock()
        mock_response.headers.get_content_charset.return_value = "utf-8"
        mock_open.return_value = mock_response

        obj = m3u8.load(playlists.SIMPLE_PLAYLIST_URI)
        self.assertIsInstance(obj, m3u8.M3U8)
        self.assertEqual(obj.target_duration, 5220)
        self.assertEqual(obj.segments[0].uri, "http://media.example.com/entire.ts")
        mock_open.assert_called_once_with(playlists.SIMPLE_PLAYLIST_URI, timeout=None)
        mock_response.geturl.assert_called_once_with()
        mock_response.info.assert_called_once_with()
        mock_response.read.assert_called_once_with()
        mock_response.headers.get_content_charset.assert_called_once_with(
            failobj="utf-8"
        )

    @patch("urllib.request.OpenerDirector.open")
    def test_load_should_remember_redirect(self, mock_open):
        mock_response = Mock(spec=HTTPResponse)
        mock_response.read.return_value = playlists.SIMPLE_PLAYLIST.encode("utf-8")
        mock_response.info.return_value = {}
        mock_response.geturl.return_value = playlists.SIMPLE_PLAYLIST_URI
        mock_response.headers = Mock()
        mock_response.headers.get_content_charset.return_value = "utf-8"
        mock_open.return_value = mock_response

        obj = m3u8.load(playlists.REDIRECT_PLAYLIST_URI)
        urlparsed = urllib.parse.urlparse(playlists.SIMPLE_PLAYLIST_URI)
        self.assertEqual(
            obj.base_uri, urlparsed.scheme + "://" + urlparsed.netloc + "/"
        )
        mock_open.assert_called_once_with(playlists.REDIRECT_PLAYLIST_URI, timeout=None)
        mock_response.geturl.assert_called_once_with()
        mock_response.info.assert_called_once_with()
        mock_response.read.assert_called_once_with()
        mock_response.headers.get_content_charset.assert_called_once_with(
            failobj="utf-8"
        )

    def test_load_should_create_object_from_file_with_relative_segments(self):
        base_uri = os.path.dirname(playlists.RELATIVE_PLAYLIST_FILENAME)
        obj = m3u8.load(playlists.RELATIVE_PLAYLIST_FILENAME)
        expected_key_abspath = f"{os.path.dirname(base_uri)}/key.bin"
        expected_key_path = "../key.bin"
        expected_ts1_abspath = "/entire1.ts"
        expected_ts1_path = "/entire1.ts"
        expected_ts2_abspath = f"{os.path.dirname(base_uri)}/entire2.ts"
        expected_ts2_path = "../entire2.ts"
        expected_ts3_abspath = (
            f"{os.path.dirname(os.path.dirname(base_uri))}/entire3.ts"
        )
        expected_ts3_path = "../../entire3.ts"
        expected_ts4_abspath = f"{base_uri}/entire4.ts"
        expected_ts4_path = "entire4.ts"
        expected_ts5_abspath = f"{base_uri}/entire5.ts"
        expected_ts5_path = "./entire5.ts"
        expected_ts6_abspath = f"{base_uri}/entire6.ts"
        expected_ts6_path = ".//entire6.ts"

        self.assertIsInstance(obj, m3u8.M3U8)
        self.assertEqual(obj.keys[0].uri, expected_key_path)
        self.assertEqual(obj.keys[0].absolute_uri, expected_key_abspath)
        self.assertEqual(obj.segments[0].uri, expected_ts1_path)
        self.assertEqual(obj.segments[0].absolute_uri, expected_ts1_abspath)
        self.assertEqual(obj.segments[1].uri, expected_ts2_path)
        self.assertEqual(obj.segments[1].absolute_uri, expected_ts2_abspath)
        self.assertEqual(obj.segments[2].uri, expected_ts3_path)
        self.assertEqual(obj.segments[2].absolute_uri, expected_ts3_abspath)
        self.assertEqual(obj.segments[3].uri, expected_ts4_path)
        self.assertEqual(obj.segments[3].absolute_uri, expected_ts4_abspath)
        self.assertEqual(obj.segments[4].uri, expected_ts5_path)
        self.assertEqual(obj.segments[4].absolute_uri, expected_ts5_abspath)
        self.assertEqual(obj.segments[5].uri, expected_ts6_path)
        self.assertEqual(obj.segments[5].absolute_uri, expected_ts6_abspath)

    @patch("urllib.request.OpenerDirector.open")
    def test_load_should_create_object_from_uri_with_relative_segments(self, mock_open):
        with open(playlists.RELATIVE_PLAYLIST_FILENAME, "rb") as f:
            relative_content = f.read().strip()

        mock_response = Mock(spec=HTTPResponse)
        mock_response.read.return_value = relative_content
        mock_response.info.return_value = {}
        mock_response.geturl.return_value = playlists.RELATIVE_PLAYLIST_URI
        mock_response.headers = Mock()
        mock_response.headers.get_content_charset.return_value = "utf-8"
        mock_open.return_value = mock_response

        obj = m3u8.load(playlists.RELATIVE_PLAYLIST_URI)
        urlparsed = urllib.parse.urlparse(playlists.RELATIVE_PLAYLIST_URI)
        base_uri = os.path.normpath(urlparsed.path + "/..")
        prefix = urlparsed.scheme + "://" + urlparsed.netloc
        expected_key_abspath = "{}{}key.bin".format(
            prefix,
            os.path.normpath(base_uri + "/..") + "/",
        )
        expected_key_path = "../key.bin"
        expected_ts1_abspath = "{}{}entire1.ts".format(prefix, "/")
        expected_ts1_path = "/entire1.ts"
        expected_ts2_abspath = "{}{}entire2.ts".format(
            prefix,
            os.path.normpath(base_uri + "/..") + "/",
        )
        expected_ts2_path = "../entire2.ts"
        expected_ts3_abspath = "{}{}entire3.ts".format(
            prefix,
            os.path.normpath(base_uri + "/../.."),
        )
        expected_ts3_path = "../../entire3.ts"
        expected_ts4_abspath = "{}{}entire4.ts".format(prefix, base_uri + "/")
        expected_ts4_path = "entire4.ts"
        expected_ts5_abspath = "{}{}entire5.ts".format(prefix, base_uri + "/")
        expected_ts5_path = "./entire5.ts"
        expected_ts6_abspath = "{}{}entire6.ts".format(prefix, base_uri + "/")
        expected_ts6_path = ".//entire6.ts"

        self.assertIsInstance(obj, m3u8.M3U8)
        self.assertEqual(obj.keys[0].uri, expected_key_path)
        self.assertEqual(obj.keys[0].absolute_uri, expected_key_abspath)
        self.assertEqual(obj.segments[0].uri, expected_ts1_path)
        self.assertEqual(obj.segments[0].absolute_uri, expected_ts1_abspath)
        self.assertEqual(obj.segments[1].uri, expected_ts2_path)
        self.assertEqual(obj.segments[1].absolute_uri, expected_ts2_abspath)
        self.assertEqual(obj.segments[2].uri, expected_ts3_path)
        self.assertEqual(obj.segments[2].absolute_uri, expected_ts3_abspath)
        self.assertEqual(obj.segments[3].uri, expected_ts4_path)
        self.assertEqual(obj.segments[3].absolute_uri, expected_ts4_abspath)
        self.assertEqual(obj.segments[4].uri, expected_ts5_path)
        self.assertEqual(obj.segments[4].absolute_uri, expected_ts5_abspath)
        self.assertEqual(obj.segments[5].uri, expected_ts6_path)
        self.assertEqual(obj.segments[5].absolute_uri, expected_ts6_abspath)
        mock_open.assert_called_once_with(playlists.RELATIVE_PLAYLIST_URI, timeout=None)
        mock_response.geturl.assert_called_once_with()
        mock_response.info.assert_called_once_with()
        mock_response.read.assert_called_once_with()
        mock_response.headers.get_content_charset.assert_called_once_with(
            failobj="utf-8"
        )

    def test_there_should_not_be_absolute_uris_with_loads(self):
        with open(playlists.RELATIVE_PLAYLIST_FILENAME) as f:
            content = f.read()
        obj = m3u8.loads(content)
        with self.assertRaises(ValueError) as cm:
            obj.keys[0].absolute_uri
        self.assertEqual(
            str(cm.exception), "There can not be `absolute_uri` with no `base_uri` set"
        )

    def test_absolute_uri_should_handle_empty_base_uri_path(self):
        key = m3u8.model.Key(
            method="AES", uri="/key.bin", base_uri="http://example.com"
        )
        self.assertEqual(key.absolute_uri, "http://example.com/key.bin")

    def test_presence_of_base_uri_if_provided_when_loading_from_string(self):
        with open(playlists.RELATIVE_PLAYLIST_FILENAME) as f:
            content = f.read()
        obj = m3u8.loads(content, uri="http://media.example.com/path/playlist.m3u8")
        self.assertEqual(obj.base_uri, "http://media.example.com/path/")

    @patch("urllib.request.OpenerDirector.open")
    def test_raise_timeout_exception_if_timeout_happens_when_loading_from_uri(
        self, mock_open
    ):
        mock_open.side_effect = socket.timeout
        with self.assertRaises(socket.timeout):
            m3u8.load(playlists.TIMEOUT_SIMPLE_PLAYLIST_URI, timeout=1)
        mock_open.assert_called_once_with(
            playlists.TIMEOUT_SIMPLE_PLAYLIST_URI, timeout=1
        )

    def test_windows_paths(self):
        file_path = "C:\\HLS Video\test.m3u8"
        with patch("builtins.open") as mock_open:
            mock_open.return_value.__enter__.return_value.read.return_value = (
                playlists.WINDOWS_PLAYLIST
            )
            obj = m3u8.load(file_path)
        self.assertEqual(obj.segments[0].uri, "C:\\HLS Video\\test1.ts")
        self.assertEqual(obj.segments[0].absolute_uri, "C:\\HLS Video\\test1.ts")
        mock_open.assert_called_once_with(file_path, encoding="utf8")
        mock_open.return_value.__enter__.return_value.read.assert_called_once_with()
