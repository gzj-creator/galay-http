#!/usr/bin/env python3
"""
Generate HPACK Huffman 4-bit nibble decode table from RFC 7541 Appendix B.

Outputs C++ static const arrays suitable for inclusion in Http2Hpack.cc.
Each state has 16 entries (one per 4-bit nibble). Each entry contains:
  - next state
  - flags (EMIT, FAIL)
  - symbol (valid when EMIT is set)

Plus a separate per-state accepting array that marks whether a state is a
valid stopping point (i.e., the bits consumed so far end on a complete symbol
boundary, or the remaining unconsumed bits are valid EOS padding).

Usage:
    python3 scripts/gen_huffman_decode_table.py
"""

# RFC 7541 Appendix B - Huffman code table
# (symbol, hex_code, bit_length)
HUFFMAN_TABLE = [
    (0,   0x1ff8,     13),
    (1,   0x7fffd8,   23),
    (2,   0xfffffe2,  28),
    (3,   0xfffffe3,  28),
    (4,   0xfffffe4,  28),
    (5,   0xfffffe5,  28),
    (6,   0xfffffe6,  28),
    (7,   0xfffffe7,  28),
    (8,   0xfffffe8,  28),
    (9,   0xffffea,   24),
    (10,  0x3ffffffc, 30),
    (11,  0xfffffe9,  28),
    (12,  0xfffffea,  28),
    (13,  0x3ffffffd, 30),
    (14,  0xfffffeb,  28),
    (15,  0xfffffec,  28),
    (16,  0xfffffed,  28),
    (17,  0xfffffee,  28),
    (18,  0xfffffef,  28),
    (19,  0xffffff0,  28),
    (20,  0xffffff1,  28),
    (21,  0xffffff2,  28),
    (22,  0x3ffffffe, 30),
    (23,  0xffffff3,  28),
    (24,  0xffffff4,  28),
    (25,  0xffffff5,  28),
    (26,  0xffffff6,  28),
    (27,  0xffffff7,  28),
    (28,  0xffffff8,  28),
    (29,  0xffffff9,  28),
    (30,  0xffffffa,  28),
    (31,  0xffffffb,  28),
    (32,  0x14,       6),
    (33,  0x3f8,      10),
    (34,  0x3f9,      10),
    (35,  0xffa,      12),
    (36,  0x1ff9,     13),
    (37,  0x15,       6),
    (38,  0xf8,       8),
    (39,  0x7fa,      11),
    (40,  0x3fa,      10),
    (41,  0x3fb,      10),
    (42,  0xf9,       8),
    (43,  0x7fb,      11),
    (44,  0xfa,       8),
    (45,  0x16,       6),
    (46,  0x17,       6),
    (47,  0x18,       6),
    (48,  0x0,        5),
    (49,  0x1,        5),
    (50,  0x2,        5),
    (51,  0x19,       6),
    (52,  0x1a,       6),
    (53,  0x1b,       6),
    (54,  0x1c,       6),
    (55,  0x1d,       6),
    (56,  0x1e,       6),
    (57,  0x1f,       6),
    (58,  0x5c,       7),
    (59,  0xfb,       8),
    (60,  0x7ffc,     15),
    (61,  0x20,       6),
    (62,  0xffb,      12),
    (63,  0x3fc,      10),
    (64,  0x1ffa,     13),
    (65,  0x21,       6),
    (66,  0x5d,       7),
    (67,  0x5e,       7),
    (68,  0x5f,       7),
    (69,  0x60,       7),
    (70,  0x61,       7),
    (71,  0x62,       7),
    (72,  0x63,       7),
    (73,  0x64,       7),
    (74,  0x65,       7),
    (75,  0x66,       7),
    (76,  0x67,       7),
    (77,  0x68,       7),
    (78,  0x69,       7),
    (79,  0x6a,       7),
    (80,  0x6b,       7),
    (81,  0x6c,       7),
    (82,  0x6d,       7),
    (83,  0x6e,       7),
    (84,  0x6f,       7),
    (85,  0x70,       7),
    (86,  0x71,       7),
    (87,  0x72,       7),
    (88,  0xfc,       8),
    (89,  0x73,       7),
    (90,  0xfd,       8),
    (91,  0x1ffb,     13),
    (92,  0x7fff0,    19),
    (93,  0x1ffc,     13),
    (94,  0x3ffc,     14),
    (95,  0x22,       6),
    (96,  0x7ffd,     15),
    (97,  0x3,        5),
    (98,  0x23,       6),
    (99,  0x4,        5),
    (100, 0x24,       6),
    (101, 0x5,        5),
    (102, 0x25,       6),
    (103, 0x26,       6),
    (104, 0x27,       6),
    (105, 0x6,        5),
    (106, 0x74,       7),
    (107, 0x75,       7),
    (108, 0x28,       6),
    (109, 0x29,       6),
    (110, 0x2a,       6),
    (111, 0x7,        5),
    (112, 0x2b,       6),
    (113, 0x76,       7),
    (114, 0x2c,       6),
    (115, 0x8,        5),
    (116, 0x9,        5),
    (117, 0x2d,       6),
    (118, 0x77,       7),
    (119, 0x78,       7),
    (120, 0x79,       7),
    (121, 0x7a,       7),
    (122, 0x7b,       7),
    (123, 0x7ffe,     15),
    (124, 0x7fc,      11),
    (125, 0x3ffd,     14),
    (126, 0x1ffd,     13),
    (127, 0xffffffc,  28),
    (128, 0xfffe6,    20),
    (129, 0x3fffd2,   22),
    (130, 0xfffe7,    20),
    (131, 0xfffe8,    20),
    (132, 0x3fffd3,   22),
    (133, 0x3fffd4,   22),
    (134, 0x3fffd5,   22),
    (135, 0x7fffd9,   23),
    (136, 0x3fffd6,   22),
    (137, 0x7fffda,   23),
    (138, 0x7fffdb,   23),
    (139, 0x7fffdc,   23),
    (140, 0x7fffdd,   23),
    (141, 0x7fffde,   23),
    (142, 0xffffeb,   24),
    (143, 0x7fffdf,   23),
    (144, 0xffffec,   24),
    (145, 0xffffed,   24),
    (146, 0x3fffd7,   22),
    (147, 0x7fffe0,   23),
    (148, 0xffffee,   24),
    (149, 0x7fffe1,   23),
    (150, 0x7fffe2,   23),
    (151, 0x7fffe3,   23),
    (152, 0x7fffe4,   23),
    (153, 0x1fffdc,   21),
    (154, 0x3fffd8,   22),
    (155, 0x7fffe5,   23),
    (156, 0x3fffd9,   22),
    (157, 0x7fffe6,   23),
    (158, 0x7fffe7,   23),
    (159, 0xffffef,   24),
    (160, 0x3fffda,   22),
    (161, 0x1fffdd,   21),
    (162, 0xfffe9,    20),
    (163, 0x3fffdb,   22),
    (164, 0x3fffdc,   22),
    (165, 0x7fffe8,   23),
    (166, 0x7fffe9,   23),
    (167, 0x1fffde,   21),
    (168, 0x7fffea,   23),
    (169, 0x3fffdd,   22),
    (170, 0x3fffde,   22),
    (171, 0xfffff0,   24),
    (172, 0x1fffdf,   21),
    (173, 0x3fffdf,   22),
    (174, 0x7fffeb,   23),
    (175, 0x7fffec,   23),
    (176, 0x1fffe0,   21),
    (177, 0x1fffe1,   21),
    (178, 0x3fffe0,   22),
    (179, 0x1fffe2,   21),
    (180, 0x7fffed,   23),
    (181, 0x3fffe1,   22),
    (182, 0x7fffee,   23),
    (183, 0x7fffef,   23),
    (184, 0xfffea,    20),
    (185, 0x3fffe2,   22),
    (186, 0x3fffe3,   22),
    (187, 0x3fffe4,   22),
    (188, 0x7ffff0,   23),
    (189, 0x3fffe5,   22),
    (190, 0x3fffe6,   22),
    (191, 0x7ffff1,   23),
    (192, 0x3ffffe0,  26),
    (193, 0x3ffffe1,  26),
    (194, 0xfffeb,    20),
    (195, 0x7fff1,    19),
    (196, 0x3fffe7,   22),
    (197, 0x7ffff2,   23),
    (198, 0x3fffe8,   22),
    (199, 0x1ffffec,  25),
    (200, 0x3ffffe2,  26),
    (201, 0x3ffffe3,  26),
    (202, 0x3ffffe4,  26),
    (203, 0x7ffffde,  27),
    (204, 0x7ffffdf,  27),
    (205, 0x3ffffe5,  26),
    (206, 0xfffff1,   24),
    (207, 0x1ffffed,  25),
    (208, 0x7fff2,    19),
    (209, 0x1fffe3,   21),
    (210, 0x3ffffe6,  26),
    (211, 0x7ffffe0,  27),
    (212, 0x7ffffe1,  27),
    (213, 0x3ffffe7,  26),
    (214, 0x7ffffe2,  27),
    (215, 0xfffff2,   24),
    (216, 0x1fffe4,   21),
    (217, 0x1fffe5,   21),
    (218, 0x3ffffe8,  26),
    (219, 0x3ffffe9,  26),
    (220, 0xffffffd,  28),
    (221, 0x7ffffe3,  27),
    (222, 0x7ffffe4,  27),
    (223, 0x7ffffe5,  27),
    (224, 0xfffec,    20),
    (225, 0xfffff3,   24),
    (226, 0xfffed,    20),
    (227, 0x1fffe6,   21),
    (228, 0x3fffe9,   22),
    (229, 0x1fffe7,   21),
    (230, 0x1fffe8,   21),
    (231, 0x7ffff3,   23),
    (232, 0x3fffea,   22),
    (233, 0x3fffeb,   22),
    (234, 0x1ffffee,  25),
    (235, 0x1ffffef,  25),
    (236, 0xfffff4,   24),
    (237, 0xfffff5,   24),
    (238, 0x3ffffea,  26),
    (239, 0x7ffff4,   23),
    (240, 0x3ffffeb,  26),
    (241, 0x7ffffe6,  27),
    (242, 0x3ffffec,  26),
    (243, 0x3ffffed,  26),
    (244, 0x7ffffe7,  27),
    (245, 0x7ffffe8,  27),
    (246, 0x7ffffe9,  27),
    (247, 0x7ffffea,  27),
    (248, 0x7ffffeb,  27),
    (249, 0xffffffe,  28),
    (250, 0x7ffffec,  27),
    (251, 0x7ffffed,  27),
    (252, 0x7ffffee,  27),
    (253, 0x7ffffef,  27),
    (254, 0x7fffff0,  27),
    (255, 0x3ffffee,  26),
    (256, 0x3fffffff, 30),  # EOS
]


class TrieNode:
    __slots__ = ('children', 'symbol', 'is_eos')

    def __init__(self):
        self.children = [None, None]  # 0, 1
        self.symbol = None  # None means not a leaf
        self.is_eos = False


def build_trie():
    """Build a binary trie from the Huffman table."""
    root = TrieNode()
    for sym, code, bits in HUFFMAN_TABLE:
        node = root
        for i in range(bits - 1, -1, -1):
            bit = (code >> i) & 1
            if node.children[bit] is None:
                node.children[bit] = TrieNode()
            node = node.children[bit]
        if sym == 256:
            node.is_eos = True
        else:
            node.symbol = sym
    return root


def is_accepting_node(node, root):
    """
    Check if this trie node is a valid stopping point for Huffman decoding.

    Per RFC 7541 Section 5.2: padding consists of the most-significant bits
    of the EOS code (which is all 1-bits). A state is accepting if:
    - It is the root (complete symbol boundary), OR
    - Following only 1-bits from here leads to EOS without passing through
      any non-EOS symbol leaf. We walk up to 30 bits (EOS code length).

    The key insight: EOS = 0x3fffffff (30 bits, all 1s). So from any node
    on the all-1s path from root, following 1-bits will eventually reach EOS.
    A node is accepting if it lies on this all-1s path and no real symbol
    is encountered between it and EOS.
    """
    if node is root:
        return True

    cur = node
    for _ in range(30):  # EOS is at most 30 bits deep
        if cur.children[1] is None:
            return False
        cur = cur.children[1]
        if cur.symbol is not None:
            # Hit a real symbol via padding bits - invalid padding
            return False
        if cur.is_eos:
            return True
    return False


def generate_decode_table(root):
    """
    Generate a 4-bit nibble state machine from the binary trie.

    Returns (table, num_states, accepting) where:
    - table[state][nibble] = (next_state, flags, symbol)
    - accepting[state] = True if state is a valid end-of-input position
    """
    node_to_state = {}
    state_to_node = []
    table = []
    accepting = []

    def get_state(node):
        nid = id(node)
        if nid not in node_to_state:
            idx = len(state_to_node)
            node_to_state[nid] = idx
            state_to_node.append(node)
            table.append([None] * 16)
            accepting.append(is_accepting_node(node, root))
        return node_to_state[nid]

    get_state(root)

    HUFFMAN_EMIT = 0x01
    HUFFMAN_FAIL = 0x04

    queue = [0]
    processed = set()

    while queue:
        state_idx = queue.pop(0)
        if state_idx in processed:
            continue
        processed.add(state_idx)

        node = state_to_node[state_idx]

        for nibble in range(16):
            cur = node
            emit_sym = None
            failed = False

            for bit_pos in range(3, -1, -1):
                bit = (nibble >> bit_pos) & 1

                if cur.children[bit] is None:
                    failed = True
                    break

                cur = cur.children[bit]

                if cur.symbol is not None:
                    emit_sym = cur.symbol
                    cur = root
                elif cur.is_eos:
                    failed = True
                    break

            if failed:
                table[state_idx][nibble] = (0, HUFFMAN_FAIL, 0)
                continue

            flags = 0
            sym = 0

            if emit_sym is not None:
                flags |= HUFFMAN_EMIT
                sym = emit_sym

            next_state = get_state(cur)
            table[state_idx][nibble] = (next_state, flags, sym)

            if next_state not in processed:
                queue.append(next_state)

    return table, len(state_to_node), accepting


def main():
    import sys

    root = build_trie()
    table, num_states, accepting = generate_decode_table(root)

    lines = []
    lines.append("// Auto-generated by scripts/gen_huffman_decode_table.py")
    lines.append(f"// {num_states} states, 16 entries per state")
    lines.append("//")
    lines.append("// Entry flags: HUFFMAN_EMIT=0x01, HUFFMAN_FAIL=0x04")
    lines.append("// State accepting: true = valid end-of-input position")
    lines.append("")
    lines.append(f"static const HuffmanDecodeEntry kHuffmanDecodeTable[{num_states}][16] = {{")

    for state_idx in range(num_states):
        lines.append(f"    /* state {state_idx} */ {{")
        entries = []
        for nibble in range(16):
            next_state, flags, sym = table[state_idx][nibble]
            flag_parts = []
            if flags & 0x04:
                flag_parts.append("HUFFMAN_FAIL")
            if flags & 0x01:
                flag_parts.append("HUFFMAN_EMIT")
            if not flag_parts:
                flag_parts.append("HUFFMAN_NONE")
            flag_str = " | ".join(flag_parts)
            entries.append(f"        {{{next_state:3d}, {flag_str}, {sym:3d}}}")
        lines.append(",\n".join(entries))
        lines.append("    },")

    lines.append("};")
    lines.append("")

    # Output per-state accepting flags
    lines.append(f"static const bool kHuffmanDecodeAccepting[{num_states}] = {{")
    row = []
    for state_idx in range(num_states):
        row.append("true" if accepting[state_idx] else "false")
        if len(row) == 16 or state_idx == num_states - 1:
            prefix = "    "
            lines.append(prefix + ", ".join(row) + ",")
            row = []
    lines.append("};")
    lines.append("")

    print("\n".join(lines))

    # ==================== Verification ====================

    HUFFMAN_EMIT = 0x01
    HUFFMAN_FAIL = 0x04

    def huffman_encode(text):
        """Encode text using the Huffman table, return list of bytes."""
        buffer = 0
        nbits = 0
        result = []
        for ch in text:
            sym = ord(ch)
            for s, code, bits in HUFFMAN_TABLE:
                if s == sym:
                    buffer = (buffer << bits) | code
                    nbits += bits
                    break
            while nbits >= 8:
                nbits -= 8
                result.append((buffer >> nbits) & 0xFF)
        if nbits > 0:
            buffer = (buffer << (8 - nbits)) | ((1 << (8 - nbits)) - 1)
            result.append(buffer & 0xFF)
        return result

    def huffman_decode(encoded_bytes):
        """Decode bytes using the generated table. Returns (decoded_str, ok)."""
        state = 0
        decoded = []
        for byte in encoded_bytes:
            hi = byte >> 4
            lo = byte & 0x0F

            entry = table[state][hi]
            if entry[1] & HUFFMAN_FAIL:
                return "", False
            if entry[1] & HUFFMAN_EMIT:
                decoded.append(chr(entry[2]))
            state = entry[0]

            entry = table[state][lo]
            if entry[1] & HUFFMAN_FAIL:
                return "", False
            if entry[1] & HUFFMAN_EMIT:
                decoded.append(chr(entry[2]))
            state = entry[0]

        if not accepting[state]:
            return "", False
        return "".join(decoded), True

    # Test 1: "www.example.com"
    test_input = "www.example.com"
    encoded = huffman_encode(test_input)
    decoded, ok = huffman_decode(encoded)
    assert ok and decoded == test_input, \
        f"FAIL: '{test_input}' -> encode -> decode = '{decoded}' (ok={ok})"
    print(f"// Verification: '{test_input}' round-trip OK", file=sys.stderr)

    # Test 2: all printable ASCII individually
    for ch_code in range(32, 127):
        ch = chr(ch_code)
        encoded = huffman_encode(ch)
        decoded, ok = huffman_decode(encoded)
        assert ok and decoded == ch, \
            f"FAIL: char {ch_code} ('{ch}') -> '{decoded}' (ok={ok})"
    print("// All printable ASCII round-trip: OK", file=sys.stderr)

    # Test 3: all single bytes 0-255
    for ch_code in range(256):
        ch = chr(ch_code)
        encoded = huffman_encode(ch)
        decoded, ok = huffman_decode(encoded)
        assert ok and decoded == ch, \
            f"FAIL: byte {ch_code} -> '{decoded}' (ok={ok})"
    print("// All single bytes 0-255 round-trip: OK", file=sys.stderr)

    # Test 4: empty input
    decoded, ok = huffman_decode([])
    assert ok and decoded == "", f"FAIL: empty input -> '{decoded}' (ok={ok})"
    print("// Empty input: OK", file=sys.stderr)

    # Test 5: longer strings
    for test_str in [
        "custom-key",
        "custom-value",
        ":method",
        "GET",
        "/sample/path",
        "no-cache",
        "Mon, 21 Oct 2013 20:13:21 GMT",
        "https://www.example.com",
    ]:
        encoded = huffman_encode(test_str)
        decoded, ok = huffman_decode(encoded)
        assert ok and decoded == test_str, \
            f"FAIL: '{test_str}' -> '{decoded}' (ok={ok})"
    print("// Longer string round-trips: OK", file=sys.stderr)

    print("// All verification tests passed!", file=sys.stderr)


if __name__ == "__main__":
    main()
