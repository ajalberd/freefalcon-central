"""LZSS codec matching src/utils/lzss.cpp (blocked-I/O, 12-bit index, 4-bit length).

The campaign header blob inside a .cam/.tac is compressed with this exact
variant -- Nelson & Gailly's blocked LZSS, where every group of eight tokens is
preceded by a flag byte.  A set flag bit means "the next byte is a literal"; a
clear one means "the next two bytes are a (position, length) pair".

Expansion here mirrors LZSS_Expand byte for byte, including the trailing-match
clamp that the engine added in 1998 (see the SCR comment in lzss.cpp): a match
that would overrun the declared output length ends the stream instead.

Compression deliberately does NOT reimplement the engine's binary-tree matcher.
It emits an all-literal stream, which LZSS_Expand decodes correctly and which
costs one flag byte per eight payload bytes (~12.5% growth).  The campaign
header is a few kB, the game re-saves it compressed properly on its first save,
and an all-literal encoder cannot produce the subtly-wrong back-reference that a
hand-rolled matcher can.
"""

INDEX_BIT_COUNT = 12
LENGTH_BIT_COUNT = 4
WINDOW_SIZE = 1 << INDEX_BIT_COUNT
WINDOW_MASK = WINDOW_SIZE - 1
BREAK_EVEN = (1 + INDEX_BIT_COUNT + LENGTH_BIT_COUNT) // 9


def expand(src, out_len):
    """Decompress `src` into exactly `out_len` bytes."""
    if out_len <= 0:
        return b""

    window = bytearray(WINDOW_SIZE)
    out = bytearray()
    ip = 0
    flags = src[ip]
    ip += 1
    mask = 1
    cur = 1
    remaining = out_len

    while remaining > 0:
        if mask == 0x100:
            flags = src[ip]
            ip += 1
            mask = 1
        bit = flags & mask
        mask <<= 1

        if bit:
            c = src[ip]
            ip += 1
            out.append(c)
            remaining -= 1
            window[cur] = c
            cur = (cur + 1) & WINDOW_MASK
        else:
            packed = src[ip]
            ip += 1
            pos = src[ip] | ((packed & 0xF) << 8)
            ip += 1
            length = (packed >> 4) + BREAK_EVEN

            if length < remaining:
                remaining -= length + 1
            else:
                # End case: the encoder's last match ran past the output buffer.
                remaining = 0
                length = -1

            for i in range(length + 1):
                c = window[(pos + i) & WINDOW_MASK]
                out.append(c)
                window[cur] = c
                cur = (cur + 1) & WINDOW_MASK

    return bytes(out[:out_len])


def compress_literal(data):
    """Encode `data` with no back-references at all.

    Correct by construction and about 12.5% larger than the input. Kept as the
    reference the real encoder is checked against.
    """
    out = bytearray()
    for base in range(0, len(data), 8):
        chunk = data[base:base + 8]
        out.append((1 << len(chunk)) - 1)   # every token in this block is a literal
        out.extend(chunk)
    return bytes(out)


MIN_MATCH = BREAK_EVEN + 1            # 2: shorter than this costs more than it saves
MAX_MATCH = MIN_MATCH + 15            # the length nibble holds 0..15
CHAIN_LIMIT = 24                      # candidates tried per position


def compress(data):
    """Encode `data` as a real LZSS stream that LZSS_Expand decodes.

    Greedy longest-match against the previous 4096 bytes, the same window the
    decoder rebuilds as it goes. The engine's own encoder uses a binary tree
    and may pick different matches; that does not matter, because any stream
    the decoder accepts is valid. What does matter is the window arithmetic:
    the decoder addresses history by `position & 4095` and writes each byte it
    copies back into the window, so a match may legally overlap the bytes it
    is still producing. That is why match_at() is allowed to read past the
    current end of `data[:i]`.
    """
    n = len(data)
    if n == 0:
        return b""

    out = bytearray()
    flags = 0
    nflags = 0
    block = bytearray()

    def emit_literal(byte):
        nonlocal flags, nflags
        flags |= 1 << nflags
        block.append(byte)
        nflags += 1
        flush_if_full()

    def emit_pair(pos, length):
        nonlocal flags, nflags
        block.append(((length - MIN_MATCH) << 4) | ((pos >> 8) & 0x0F))
        block.append(pos & 0xFF)
        nflags += 1
        flush_if_full()

    def flush_if_full():
        nonlocal flags, nflags
        if nflags == 8:
            out.append(flags)
            out.extend(block)
            del block[:]
            flags = 0
            nflags = 0

    # 3-byte prefix -> recent absolute positions, most recent first.
    chains = {}

    def match_at(cand, i):
        """How many bytes at data[i:] the decoder would reproduce from `cand`."""
        limit = min(MAX_MATCH, n - i)
        k = 0
        while k < limit and data[cand + k] == data[i + k]:
            k += 1
        return k

    i = 0
    while i < n:
        best_len = 0
        best_pos = 0
        if i + MIN_MATCH <= n:
            key = data[i:i + 3]
            bucket = chains.get(key)
            if bucket:
                lowest = i - (WINDOW_SIZE - 1)
                for cand in bucket:
                    if cand < lowest:
                        break
                    k = match_at(cand, i)
                    if k > best_len:
                        best_len = k
                        best_pos = cand
                        if k == MAX_MATCH:
                            break

        if best_len >= MIN_MATCH:
            # The decoder's window index for this history position. Its window
            # is seeded with current_position = 1, so history byte h lives at
            # (h + 1) & 4095.
            emit_pair((best_pos + 1) & WINDOW_MASK, best_len)
            step = best_len
        else:
            emit_literal(data[i])
            step = 1

        for j in range(i, i + step):
            if j + 3 <= n:
                bucket = chains.setdefault(data[j:j + 3], [])
                bucket.insert(0, j)
                if len(bucket) > CHAIN_LIMIT:
                    del bucket[CHAIN_LIMIT:]
        i += step

    if nflags:
        out.append(flags)
        out.extend(block)
    return bytes(out)
