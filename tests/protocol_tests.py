"""Exercise framing failures without starting Vulkan or loading the runtime."""
import json
import struct
import subprocess
import sys
import unittest

WORKER = sys.argv.pop(1)
MAX_FRAME_BYTES = 64 * 1024 * 1024
MORE_FRAMES = 0x80000000


def frame(message, chunk_size=MAX_FRAME_BYTES):
    return chain(json.dumps(message, ensure_ascii=False).encode(), chunk_size)


def chain(payload, chunk_size=MAX_FRAME_BYTES):
    chunks = []
    for offset in range(0, len(payload), chunk_size):
        chunk = payload[offset:offset + chunk_size]
        flags = len(chunk) | (MORE_FRAMES if offset + len(chunk) < len(payload) else 0)
        chunks.append(struct.pack("<I", flags) + chunk)
    return b"".join(chunks)


def fnv(data):
    h = 14695981039346656037
    for byte in data:
        h = ((h ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def replies(stream):
    messages = []
    payload = bytearray()
    while stream:
        flags, = struct.unpack("<I", stream[:4])
        size = flags & ~MORE_FRAMES
        assert 0 < size <= MAX_FRAME_BYTES and len(stream) >= 4 + size
        payload.extend(stream[4:4 + size])
        stream = stream[4 + size:]
        if not flags & MORE_FRAMES:
            messages.append(json.loads(payload))
            payload.clear()
    assert not payload, "Incomplete response"
    return messages


class ProtocolTests(unittest.TestCase):
    def run_worker(self, payload):
        return subprocess.run([WORKER], input=payload, capture_output=True, timeout=30)

    def test_session_and_recoverable_error(self):
        messages = [{"id": 1, "version": 3, "op": "hello"},
                    {"id": 2, "version": 3, "op": "unknown"},
                    {"id": 3, "version": 3, "op": "shutdown"}]
        result = self.run_worker(b"".join(map(frame, messages)))
        self.assertEqual(result.returncode, 0, result.stderr)
        responses = replies(result.stdout)
        self.assertEqual([r["id"] for r in responses], [1, 2, 3])
        self.assertEqual([r["ok"] for r in responses], [True, False, True])
        self.assertEqual(responses[0]["result"]["protocol"], 3)

    def test_large_transfer_and_reply(self):
        # A large unknown operation is echoed in the error. This exercises both
        # directions past 64 MiB without requiring a GPU or Remix runtime.
        operation = "x" * (MAX_FRAME_BYTES + 1024) + "終"
        payload = frame({"version": 3, "op": operation, "id": 7})
        payload += frame({"version": 3, "op": "hello", "id": 8})
        result = self.run_worker(payload)
        self.assertEqual(result.returncode, 0, result.stderr)
        responses = replies(result.stdout)
        self.assertEqual([r["id"] for r in responses], [7, 8])
        self.assertEqual(responses[0]["error"], "Unknown renderer operation: " + operation)
        self.assertTrue(responses[1]["ok"])

    def test_split_utf8(self):
        message = {"version": 3, "op": "材質🌲", "id": 9}
        result = self.run_worker(frame(message, chunk_size=1))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(replies(result.stdout)[0]["error"], "Unknown renderer operation: 材質🌲")

    def test_binary_buffers(self):
        # Binary values follow the JSON chain as one concatenated chain.
        blobs = [struct.pack("<3f", 1.5, -2.0, 3.25), struct.pack("<i", -7), b"", bytes(range(256)) * 3]
        message = {"id": 1, "version": 3, "op": "inspect_buffers", "buffers": [len(b) for b in blobs],
                   "data": {"a": {"$buffer": 0, "type": "vec3"}, "b": [{"$buffer": 1, "type": "i32"},
                            {"$buffer": 2, "type": "bytes"}, {"$buffer": 3, "type": "u32"}]}}
        big = bytes(range(251)) * (MAX_FRAME_BYTES // 251 + 1)
        large = {"id": 2, "version": 3, "op": "inspect_buffers", "buffers": [len(big)], "data": [{"$buffer": 0, "type": "bytes"}]}
        payload = frame(message) + chain(b"".join(blobs), chunk_size=5) + frame(large) + chain(big)
        payload += frame({"id": 3, "version": 3, "op": "hello"})
        result = self.run_worker(payload)
        self.assertEqual(result.returncode, 0, result.stderr)
        responses = replies(result.stdout)
        self.assertEqual([r["id"] for r in responses], [1, 2, 3])
        self.assertEqual(responses[0]["result"], [{"type": t, "size": len(b), "hash": fnv(b)} for t, b in zip(["vec3", "i32", "bytes", "u32"], blobs)])
        self.assertEqual(responses[1]["result"][0]["size"], len(big))
        self.assertTrue(responses[2]["ok"])

    def test_malformed_buffers(self):
        def declared(sizes, data):
            return frame({"version": 3, "op": "inspect_buffers", "buffers": sizes, "data": data})
        ref = {"$buffer": 0, "type": "bytes"}
        for payload in [declared([4], ref),
                        declared([4], ref) + chain(b"abc"),
                        declared([4], ref) + chain(b"abcde"),
                        declared([4], {"$buffer": 1, "type": "bytes"}) + chain(b"abcd"),
                        declared([4], {"$buffer": 0, "type": "unknown"}) + chain(b"abcd")]:
            with self.subTest(payload=payload[:64]):
                result = self.run_worker(payload)
                self.assertEqual(result.returncode, 1)
                self.assertTrue(result.stderr)

    def test_malformed_input(self):
        unfinished = struct.pack("<I", MORE_FRAMES | 1) + b"{"
        for payload in [struct.pack("<I", 0), struct.pack("<I", MAX_FRAME_BYTES + 1),
                        struct.pack("<I", 40) + b"{}", b"\x01\x00",
                        frame({"version": 1, "op": "hello"}), struct.pack("<I", 1) + b"{",
                        unfinished, unfinished + b"\x01\x00",
                        unfinished + struct.pack("<I", 5) + b"}",
                        unfinished + struct.pack("<I", MORE_FRAMES),
                        unfinished + struct.pack("<I", MAX_FRAME_BYTES + 1)]:
            with self.subTest(payload=payload):
                result = self.run_worker(payload)
                self.assertEqual(result.returncode, 1)
                self.assertTrue(result.stderr)


if __name__ == "__main__":
    unittest.main()
