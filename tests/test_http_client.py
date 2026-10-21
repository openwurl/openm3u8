import gzip
import urllib.request
from http.client import HTTPResponse
from unittest import TestCase
from unittest.mock import ANY, Mock, patch

from openm3u8.httpclient import DefaultHTTPClient


class DefaultHTTPClientTests(TestCase):
    @patch("urllib.request.OpenerDirector.open")
    def test_download_normal_content(self, mock_open):
        client = DefaultHTTPClient()
        mock_response = Mock(spec=HTTPResponse)
        mock_response.read.return_value = b"playlist content"
        mock_response.info.return_value = {}
        mock_response.geturl.return_value = "http://example.com/index.m3u8"
        mock_response.headers = Mock()
        mock_response.headers.get_content_charset.return_value = "utf-8"
        mock_open.return_value = mock_response

        content, base_uri = client.download("http://example.com/index.m3u8")

        self.assertEqual(content, "playlist content")
        self.assertEqual(base_uri, "http://example.com/")
        mock_open.assert_called_once_with("http://example.com/index.m3u8", timeout=None)
        mock_response.geturl.assert_called_once_with()
        mock_response.info.assert_called_once_with()
        mock_response.read.assert_called_once_with()
        mock_response.headers.get_content_charset.assert_called_once_with(
            failobj="utf-8"
        )

    @patch("urllib.request.OpenerDirector.open")
    def test_download_gzipped_content(self, mock_open):
        client = DefaultHTTPClient()
        original_content = "playlist gzipped content"
        gzipped_content = gzip.compress(original_content.encode("utf-8"))
        mock_response = Mock(spec=HTTPResponse)
        mock_response.read.return_value = gzipped_content
        mock_response.info.return_value = {"Content-Encoding": "gzip"}
        mock_response.geturl.return_value = "http://example.com/index.m3u8"
        mock_response.headers = Mock()
        mock_response.headers.get_content_charset.return_value = "utf-8"
        mock_open.return_value = mock_response

        content, base_uri = client.download("http://example.com/index.m3u8")

        self.assertEqual(content, original_content)
        self.assertEqual(base_uri, "http://example.com/")
        mock_open.assert_called_once_with("http://example.com/index.m3u8", timeout=None)
        mock_response.geturl.assert_called_once_with()
        mock_response.info.assert_called_once_with()
        mock_response.read.assert_called_once_with()
        mock_response.headers.get_content_charset.assert_called_once_with(
            failobj="utf-8"
        )

    @patch.object(
        urllib.request.ProxyHandler,
        "__init__",
        autospec=True,
        side_effect=urllib.request.ProxyHandler.__init__,
    )
    @patch("urllib.request.OpenerDirector.open")
    def test_download_with_proxy(self, mock_open, mock_proxy_handler_init):
        proxies = {"http": "http://proxy.example.com"}
        client = DefaultHTTPClient(proxies=proxies)
        mock_response = Mock(spec=HTTPResponse)
        mock_response.read.return_value = b"playlist proxied content"
        mock_response.info.return_value = {}
        mock_response.geturl.return_value = "http://example.com/index.m3u8"
        mock_response.headers = Mock()
        mock_response.headers.get_content_charset.return_value = "utf-8"
        mock_open.return_value = mock_response

        content, base_uri = client.download("http://example.com/index.m3u8")

        self.assertEqual(content, "playlist proxied content")
        self.assertEqual(base_uri, "http://example.com/")
        mock_proxy_handler_init.assert_called_once_with(ANY, proxies)
        mock_open.assert_called_once_with("http://example.com/index.m3u8", timeout=None)
        mock_response.geturl.assert_called_once_with()
        mock_response.info.assert_called_once_with()
        mock_response.read.assert_called_once_with()
        mock_response.headers.get_content_charset.assert_called_once_with(
            failobj="utf-8"
        )
