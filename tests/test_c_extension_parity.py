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

    def test_program_date_time_variants_and_parts_parity(self):
        # Exercises lazy PDT advancement, 2-slot timedelta cache (alternating
        # durations), per-segment PDT overrides, and #EXT-X-PART PDT flush.
        content = "\n".join(
            [
                "#EXTM3U",
                "#EXT-X-VERSION:6",
                "#EXT-X-TARGETDURATION:6",
                "#EXT-X-PROGRAM-DATE-TIME:2026-11-24T16:00:00.000Z",
                "#EXTINF:6.006,",
                "seg0.ts",
                "#EXTINF:5.994,",
                "seg1.ts",
                "#EXTINF:6.006,",
                "seg2.ts",
                "#EXT-X-PROGRAM-DATE-TIME:2026-11-24T17:00:00.500+02:00",
                "#EXTINF:4.0,",
                "seg3.ts",
                "#EXT-X-PROGRAM-DATE-TIME:2026-11-24T17:00:04.500+02:00",
                '#EXT-X-PART:DURATION=2.0,URI="seg4.0.m4s"',
                '#EXT-X-PART:DURATION=2.0,URI="seg4.1.m4s"',
                "#EXTINF:4.0,",
                "seg4.ts",
                "#EXTINF:4.0,",
                "seg5.ts",
                "#EXT-X-ENDLIST",
            ]
        )
        self.assertEqual(c_parser.parse(content), py_parser.parse(content))

    def test_program_date_time_overflow_duration_parity(self):
        content = "\n".join(
            [
                "#EXTM3U",
                "#EXT-X-PROGRAM-DATE-TIME:2026-11-24T16:00:00.000Z",
                "#EXTINF:1e20,",
                "seg0.ts",
                "#EXT-X-PROGRAM-DATE-TIME:2026-11-24T16:00:06.000Z",
                "#EXTINF:6.0,",
                "seg1.ts",
            ]
        )
        with self.assertRaises(OverflowError):
            py_parser.parse(content)
        with self.assertRaises(OverflowError):
            c_parser.parse(content)

    def test_partial_segments_and_extinf_edge_cases_parity(self):
        cases = [
            # Trailing #EXTINF with no URI
            "#EXTM3U\n#EXTINF:6.006,trailing title\n",
            # Trailing #EXT-X-BYTERANGE with no URI
            "#EXTM3U\n#EXT-X-BYTERANGE:100@0\n",
            # Trailing #EXT-X-PART with no URI
            '#EXTM3U\n#EXT-X-PART:DURATION=0.5,URI="p0.m4s"\n',
            # Duplicate consecutive #EXTINF before a single URI
            "#EXTM3U\n#EXTINF:3.0,first\n#EXTINF:6.006,second, with comma\nseg.ts\n",
            # Byterange before EXTINF vs EXTINF before byterange
            (
                "#EXTM3U\n#EXT-X-BYTERANGE:100@0\n#EXTINF:4.0,t1\ns1.ts\n"
                "#EXTINF:4.0,\n#EXT-X-BYTERANGE:200@100\ns2.ts\n"
            ),
        ]
        for content in cases:
            with self.subTest(content=content):
                self.assertEqual(c_parser.parse(content), py_parser.parse(content))

    def test_custom_tags_parser_state_sync_parity(self):
        content = "\n".join(
            [
                "#EXTM3U",
                "#EXT-X-PROGRAM-DATE-TIME:2026-11-24T16:00:00.000Z",
                "#EXTINF:6.0,",
                "#EXT-X-CUSTOM-OBSERVE:alpha",
                "seg0.ts",
                "#EXT-X-CUSTOM-SET-FLAGS",
                "#EXTINF:4.0,",
                "seg1.ts",
                "#EXT-X-ENDLIST",
            ]
        )

        def make_parser():
            observed = []

            def custom_tags_parser(line, lineno, data, state):
                if line.startswith("#EXT-X-CUSTOM-OBSERVE"):
                    observed.append(
                        (
                            state["expect_segment"],
                            state["segment"]["duration"],
                            state["current_program_date_time"],
                            state["current_key"],
                        )
                    )
                    state["segment"]["custom_parser_values"] = {"tag": "alpha"}
                    return True
                if line == "#EXT-X-CUSTOM-SET-FLAGS":
                    observed.append(("after_seg0", state["current_program_date_time"]))
                    state["discontinuity"] = True
                    state["cue_out"] = True
                    state["current_cue_out_scte35"] = "SCTE_PAYLOAD"
                    return True
                return False

            return custom_tags_parser, observed

        py_fn, py_obs = make_parser()
        c_fn, c_obs = make_parser()
        py_res = py_parser.parse(content, custom_tags_parser=py_fn)
        c_res = c_parser.parse(content, custom_tags_parser=c_fn)

        self.assertEqual(c_res, py_res)
        self.assertEqual(c_obs, py_obs)

    def test_repeated_and_alternating_keys_parity(self):
        k1 = '#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/k1",IV=0x1'
        k2 = '#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/k2",IV=0x2'
        content = "\n".join(
            [
                "#EXTM3U",
                k1,
                "#EXTINF:6.0,",
                "s0.ts",
                k1,
                "#EXTINF:6.0,",
                "s1.ts",
                k2,
                "#EXTINF:6.0,",
                "s2.ts",
                k1,
                "#EXTINF:6.0,",
                "s3.ts",
                k1,
                "#EXTINF:6.0,",
                "s4.ts",
            ]
        )
        py_res = py_parser.parse(content)
        c_res = c_parser.parse(content)
        self.assertEqual(c_res, py_res)
        self.assertEqual(len(c_res["keys"]), 2)
        # Cached key copies must not alias one another on mutation
        c_res["segments"][0]["key"]["uri"] = "mutated"
        self.assertEqual(c_res["segments"][1]["key"]["uri"], "https://example.com/k1")

    def test_long_cue_out_and_duration_clearing_parity(self):
        long_cue = "A" * 1200
        content = "\n".join(
            [
                "#EXTM3U",
                f'#EXT-X-CUE-OUT:CUE="{long_cue}",DURATION=30.0',
                "#EXTINF:6.0,",
                "s0.ts",
                "#EXT-X-CUE-IN",
                "#EXTINF:6.0,",
                "s1.ts",
                '#EXT-X-CUE-OUT:CUE="SHORT"',
                "#EXTINF:6.0,",
                "s2.ts",
            ]
        )
        self.assertEqual(c_parser.parse(content), py_parser.parse(content))

    def test_line_ending_variants_parity(self):
        lines = [
            "#EXTM3U",
            "#EXT-X-VERSION:3",
            "#EXT-X-TARGETDURATION:6",
            "",
            "#EXTINF:6.006,",
            "seg0.ts",
            "#EXTINF:6.006,title",
            "seg1.ts",
            "#EXT-X-ENDLIST",
        ]
        for sep in ("\n", "\r\n", "\r"):
            content = sep.join(lines)
            with self.subTest(sep=repr(sep)):
                self.assertEqual(c_parser.parse(content), py_parser.parse(content))

        mixed = (
            "#EXTM3U\r\n#EXTINF:4.0,\rseg0.ts\n#EXTINF:5.0,\r\nseg1.ts\r#EXT-X-ENDLIST"
        )
        self.assertEqual(c_parser.parse(mixed), py_parser.parse(mixed))

    def test_float_parsing_boundary_parity(self):
        durations = [
            "0",
            "0.0",
            "000000000000000006.006",
            "6.006",
            "5.972633",
            "123456789012345",
            "1234567890123456",
            "0.0000000000000000000001",
            "0.00000000000000000000001",
            "1.5e2",
            " 1e2 ",
            " 4.25 ",
            " -1.5 ",
            " +3.5 ",
            " -2.5e-1 ",
            "+3.5",
        ]
        lines = ["#EXTM3U"]
        for idx, dur in enumerate(durations):
            lines.append(f"#EXTINF:{dur},")
            lines.append(f"seg{idx}.ts")
        content = "\n".join(lines)
        py_res = py_parser.parse(content)
        c_res = c_parser.parse(content)
        self.assertEqual(c_res, py_res)
        # Verify exact bit-for-bit equality (not just approximate)
        for py_seg, c_seg in zip(py_res["segments"], c_res["segments"]):
            self.assertEqual(py_seg["duration"].hex(), c_seg["duration"].hex())

    def test_strict_mode_extm3u_prefix_boundary_parity(self):
        content = "#EXTM3U\n#EXTM3U_UNKNOWN\n#EXTINF:6.0,\nseg.ts\n"
        with self.assertRaises(py_parser.ParseError):
            py_parser.parse(content, strict=True)
        with self.assertRaises(py_parser.ParseError):
            c_parser.parse(content, strict=True)

    def test_custom_tags_parser_mutating_data_lists_raises_typeerror(self):
        cases = [
            ("segments", "#EXTM3U\n#EXTINF:1,\nfile.ts"),
            ("playlists", "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100\nlow.m3u8"),
            ("media", '#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="g",NAME="n"'),
            ("segment_map", '#EXTM3U\n#EXT-X-MAP:URI="init.mp4"'),
            ("session_keys", '#EXTM3U\n#EXT-X-SESSION-KEY:METHOD=AES-128,URI="k"'),
        ]
        for key, content in cases:
            for bad_val in (None, ()):
                with self.subTest(key=key, bad_val=bad_val):

                    def mutator(line, lineno, data, state, k=key, v=bad_val):
                        if lineno == 1:
                            if v is None:
                                data.pop(k, None)
                            else:
                                data[k] = v
                        return False

                    with self.assertRaises(TypeError):
                        c_parser.parse(content, custom_tags_parser=mutator)

    def test_custom_tags_parser_invalid_state_types_raise_typeerror(self):
        def bad_segment(line, lineno, data, state):
            if lineno == 1:
                state["segment"] = 123
            return False

        with self.assertRaises(TypeError):
            c_parser.parse(
                "#EXTM3U\n#EXTINF:1,\nfile.ts", custom_tags_parser=bad_segment
            )

        def bad_dateranges(line, lineno, data, state):
            if lineno == 1:
                state["dateranges"] = ()
            return False

        with self.assertRaises(TypeError):
            c_parser.parse(
                '#EXTM3U\n#EXT-X-DATERANGE:ID="1",START-DATE="2026-01-01T00:00:00Z"',
                custom_tags_parser=bad_dateranges,
            )

    def test_non_finite_bandwidth_falls_back_to_string(self):
        content = (
            "#EXTM3U\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=inf\n"
            "a.m3u8\n"
            '#EXT-X-STREAM-INF:BANDWIDTH="nan"\n'
            "b.m3u8\n"
        )
        result = c_parser.parse(content)
        self.assertEqual(result["playlists"][0]["stream_info"]["bandwidth"], "inf")
        self.assertEqual(result["playlists"][1]["stream_info"]["bandwidth"], "nan")

    def test_parse_error_identity(self):
        self.assertIs(c_parser.ParseError, py_parser.ParseError)

    def test_bytes_input_raises_typeerror(self):
        with self.assertRaises(TypeError):
            c_parser.parse(b"#EXTM3U\n#EXTINF:5,\nseg.ts\n")

    def test_strict_empty_and_whitespace_parity(self):
        for content in ("", "   \n\t\r\n  "):
            with self.subTest(content=repr(content)):
                self.assertEqual(
                    c_parser.parse(content, strict=True),
                    py_parser.parse(content, strict=True),
                )

    def test_reentrant_data_mutation_during_key_and_segment_ops(self):
        class MutatingKey:
            def __init__(self, target_data):
                self._data = target_data

            def __eq__(self, other):
                # Drop the dict's reference to the original keys list mid-comparison.
                self._data["keys"] = []
                return False

        def inject_mutating_key(line, lineno, data, state):
            if lineno == 1:
                data["keys"].append(MutatingKey(data))
            return False

        key_playlist = (
            "#EXTM3U\n"
            '#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/k"\n'
            "#EXTINF:5,\n"
            "seg.ts\n"
        )
        result = c_parser.parse(key_playlist, custom_tags_parser=inject_mutating_key)
        py_result = py_parser.parse(
            key_playlist, custom_tags_parser=inject_mutating_key
        )
        self.assertEqual(result, py_result)

        class MutatingSegmentMap:
            def __init__(self, target_data):
                self._data = target_data

            def __bool__(self):
                # Drop the dict's reference to the original segments list mid-parse.
                self._data["segments"] = []
                return True

        def inject_mutating_map(line, lineno, data, state):
            if lineno == 1:
                state["current_segment_map"] = MutatingSegmentMap(data)
            return False

        seg_playlist = "#EXTM3U\n#EXTINF:5,\nseg.ts\n"
        result = c_parser.parse(seg_playlist, custom_tags_parser=inject_mutating_map)
        self.assertEqual(len(result["segments"]), 1)

    def test_reentrant_duration_float_mutation_and_error(self):
        playlist = (
            "#EXTM3U\n"
            "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00\n"
            "#EXTINF:1,\n"
            "#CUSTOM\n"
            "a.ts\n"
        )

        # Case 1: __float__ deletes itself from segment dict and returns invalid type
        def self_deleting_duration(line, lineno, data, state):
            if line == "#CUSTOM":

                class Duration:
                    def __float__(self):
                        del state["segment"]["duration"]
                        return 1  # Invalid: __float__ must return float

                state["segment"] = {"duration": Duration()}
            return False

        with self.assertRaises(TypeError):
            py_parser.parse(playlist, custom_tags_parser=self_deleting_duration)
        with self.assertRaises(TypeError):
            c_parser.parse(playlist, custom_tags_parser=self_deleting_duration)

        # Case 2: __float__ replaces data["segments"]; segment must land in the new list
        def replacing_segments_duration(line, lineno, data, state):
            if line == "#CUSTOM":

                class Duration:
                    def __float__(self):
                        data["segments"] = []
                        return 1.0

                state["segment"] = {"duration": Duration()}
            return False

        c_res = c_parser.parse(playlist, custom_tags_parser=replacing_segments_duration)
        self.assertEqual(len(c_res["segments"]), 1)
        self.assertEqual(c_res["segments"][0]["uri"], "a.ts")

    def test_reentrant_bool_mutation_in_state_sync(self):
        def mutating_bool_parser(line, lineno, data, state):
            if line == "#CUSTOM":

                class SelfDeletingFlag:
                    def __bool__(self):
                        state.pop("discontinuity", None)
                        return True

                state["discontinuity"] = SelfDeletingFlag()
            return False

        playlist = "#EXTM3U\n#EXTINF:5,\n#CUSTOM\na.ts\n"
        c_res = c_parser.parse(playlist, custom_tags_parser=mutating_bool_parser)
        self.assertTrue(c_res["segments"][0]["discontinuity"])

    def test_program_date_time_overflow_raises_in_c_and_python(self):
        playlist = (
            "#EXTM3U\n#EXT-X-PROGRAM-DATE-TIME:9999-12-31T23:59:59\n#EXTINF:2,\na.ts\n"
        )
        with self.assertRaises(OverflowError):
            py_parser.parse(playlist)
        with self.assertRaises(OverflowError):
            c_parser.parse(playlist)

    def test_rotating_keys_deduplication_parity(self):
        # Exercises lazy hash-set deduplication (>8 distinct keys) plus duplicates
        lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:6"]
        for i in range(25):
            key_id = i % 15
            lines.append(
                f'#EXT-X-KEY:METHOD=AES-128,URI="https://example.com/k{key_id}",IV=0x{key_id:032x}'
            )
            lines.append("#EXTINF:6.0,")
            lines.append(f"seg{i}.ts")
        content = "\n".join(lines) + "\n"
        c_res = c_parser.parse(content)
        py_res = py_parser.parse(content)
        self.assertEqual(c_res, py_res)
        self.assertEqual(len(c_res["keys"]), 15)
