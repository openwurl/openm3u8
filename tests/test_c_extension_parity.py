from unittest import TestCase, skipIf

import openm3u8.parser as py_parser

try:
    import openm3u8._m3u8_parser as c_parser
except ImportError:  # pragma: no cover
    c_parser = None


@skipIf(c_parser is None, "C extension not built")
class CExtensionParityTests(TestCase):
    def test_custom_tags_parser_truthiness_in_strict_mode(self):
        content = "\n".join(
            [
                "#EXTM3U",
                "#EXT-X-VERSION:3",
                "#EXT-X-TARGETDURATION:8",
                "#EXT-X-CUSTOM-TAG:foo=bar",
                "#EXTINF:8,",
                "file.ts",
                "#EXT-X-ENDLIST",
            ]
        )

        def custom_tags_parser(line, lineno, data, state):
            if line.startswith("#EXT-X-CUSTOM-TAG"):
                # Truthy but not literally True; parser.py treats this as handled.
                return 1
            return 0

        self.assertTrue(
            py_parser.parse(content, strict=True, custom_tags_parser=custom_tags_parser)
        )
        self.assertTrue(
            c_parser.parse(content, strict=True, custom_tags_parser=custom_tags_parser)
        )

    def test_custom_tags_parser_exceptions_propagate(self):
        content = "\n".join(
            [
                "#EXTM3U",
                "#EXT-X-VERSION:3",
                "#EXT-X-TARGETDURATION:8",
                "#EXT-X-CUSTOM-TAG:foo=bar",
                "#EXTINF:8,",
                "file.ts",
                "#EXT-X-ENDLIST",
            ]
        )

        def custom_tags_parser(line, lineno, data, state):
            raise RuntimeError("boom")

        with self.assertRaises(RuntimeError):
            py_parser.parse(
                content, strict=False, custom_tags_parser=custom_tags_parser
            )

        with self.assertRaises(RuntimeError):
            c_parser.parse(content, strict=False, custom_tags_parser=custom_tags_parser)

    def test_single_quoted_attribute_values_match_python(self):
        content = "\n".join(
            [
                "#EXTM3U",
                "#EXT-X-VERSION:3",
                "#EXT-X-TARGETDURATION:8",
                "#EXT-X-KEY:METHOD=AES-128,URI='https://example.com/key'",
                "#EXTINF:8,",
                "file.ts",
                "#EXT-X-ENDLIST",
            ]
        )

        py = py_parser.parse(content)
        c = c_parser.parse(content)

        self.assertEqual(py["keys"][0]["uri"], "https://example.com/key")
        self.assertEqual(c["keys"][0]["uri"], "https://example.com/key")

    def test_strict_version_matching_line_numbers_preserve_blank_lines(self):
        # Version 2 + floating-point EXTINF should error under version matching rules.
        # The blank line here must be preserved for consistent line_number reporting.
        content = "\n".join(
            [
                "#EXTM3U",
                "#EXT-X-VERSION:2",
                "",
                "#EXTINF:2.5,",
                "file.ts",
                "#EXT-X-ENDLIST",
            ]
        )

        with self.assertRaises(Exception) as py_exc:
            py_parser.parse(content, strict=True)
        with self.assertRaises(Exception) as c_exc:
            c_parser.parse(content, strict=True)

        py_errors = py_exc.exception.args[0]
        c_errors = c_exc.exception.args[0]

        self.assertIsInstance(py_errors, list)
        self.assertTrue(py_errors)
        self.assertIsInstance(c_errors, list)
        self.assertTrue(c_errors)

        self.assertEqual(py_errors[0].line_number, c_errors[0].line_number)
        self.assertEqual(py_errors[0].line, c_errors[0].line)

    def test_bare_value_tags_do_not_read_stale_line_buffer(self):
        version_result = c_parser.parse("XXXXXXXXXXXXXXX7\n#EXT-X-VERSION")
        self.assertNotIn("version", version_result)

        extinf_result = c_parser.parse("ABCDEFGH42\n#EXTINF")
        self.assertEqual(extinf_result["segments"], [])

    def test_custom_tags_parser_mutating_keys_fails_cleanly(self):
        def custom_tags_parser(line, lineno, data, state):
            if lineno == 1:
                data["keys"] = ()
            return False

        with self.assertRaises(TypeError):
            c_parser.parse(
                "#EXTM3U\n#EXTINF:1,\nfile.ts",
                custom_tags_parser=custom_tags_parser,
            )

    def test_embedded_nul_is_rejected_at_c_parser_boundary(self):
        with self.assertRaisesRegex(ValueError, "embedded null bytes"):
            c_parser.parse("#EXTM3U\n#EXT-X-VERSION:3\0#EXT-X-TARGETDURATION:8")
