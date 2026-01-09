"""
CFFI-based C parser for M3U8 playlists.

This module provides a fast C implementation of the M3U8 parser.
The C parser handles the hot loop (line scanning and state management),
and only creates Python objects at the end of parsing.
"""

from m3u8.cparser.fast_parser import parse

__all__ = ["parse"]
