"""Exercise real TCP framing, authentication and lifecycle, without HA stubs."""

import asyncio
import importlib.util
import unittest
from pathlib import Path

path = (
    Path(__file__).resolve().parents[1]
    / "custom_components/voice_pe_monitor/receiver.py"
)
spec = importlib.util.spec_from_file_location("monitor_receiver", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
KEY = "k" * 40


class ReceiverTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.receiver = module.Receiver("voice", KEY)
        self.events = asyncio.Queue()
        self.receiver.listeners.add(self.events.put_nowait)
        await self.receiver.start("127.0.0.1", 0)
        self.port = self.receiver.server.sockets[0].getsockname()[1]

    async def asyncTearDown(self):
        await self.receiver.stop()

    async def connect(self, hello=None):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        writer.write(hello or f"voice {KEY}\n".encode())
        await writer.drain()
        return reader, writer

    async def event(self):
        return await asyncio.wait_for(self.events.get(), 1)

    async def test_invalid_key_and_wrong_node_never_publish(self):
        for hello in (
            b"voice wrong\n",
            f"other {KEY}\n".encode(),
            b"bad\n",
            b"x" * 500 + b"\n",
        ):
            reader, writer = await self.connect(hello)
            self.assertEqual(await asyncio.wait_for(reader.read(), 1), b"")
            writer.close()
            await writer.wait_closed()
        self.assertTrue(self.events.empty())
        self.assertFalse(self.receiver.connected)

    async def test_partial_packets_are_reassembled_without_recording(self):
        reader, writer = await self.connect()
        self.assertEqual(await reader.readline(), b"OK\n")
        self.assertEqual(await self.event(), {"type": "status", "connected": True})
        pcm = bytes(range(256)) * 5
        writer.write(pcm[:101])
        await writer.drain()
        await asyncio.sleep(0.01)
        self.assertTrue(self.events.empty())
        writer.write(pcm[101:])
        await writer.drain()
        self.assertEqual(await self.event(), {"type": "audio", "data": pcm})
        writer.close()
        await writer.wait_closed()
        self.assertEqual(await self.event(), {"type": "status", "connected": False})
        self.assertIsNone(self.receiver._writer)

    async def test_duplicate_source_cannot_displace_active_column(self):
        reader, first = await self.connect()
        self.assertEqual(await reader.readline(), b"OK\n")
        await self.event()
        other_reader, other = await self.connect()
        self.assertEqual(await asyncio.wait_for(other_reader.read(), 1), b"")
        self.assertTrue(self.receiver.connected)
        self.assertTrue(self.events.empty())
        other.close()
        first.close()
        await other.wait_closed()
        await first.wait_closed()
        self.assertEqual(await self.event(), {"type": "status", "connected": False})

    async def test_shutdown_closes_stream_and_all_tasks(self):
        reader, writer = await self.connect()
        await reader.readline()
        await self.event()
        await self.receiver.stop()
        self.assertEqual(await asyncio.wait_for(reader.read(), 1), b"")
        self.assertFalse(self.receiver.connected)
        self.assertFalse(self.receiver._tasks)
        writer.close()
        await writer.wait_closed()

    async def test_no_history_is_replayed_to_new_listener(self):
        reader, writer = await self.connect()
        await reader.readline()
        await self.event()
        writer.write(b"\x00" * module.CHUNK)
        await writer.drain()
        await self.event()
        late = asyncio.Queue()
        self.receiver.listeners.add(late.put_nowait)
        await asyncio.sleep(0.01)
        self.assertTrue(late.empty())
        writer.close()
        await writer.wait_closed()


if __name__ == "__main__":
    unittest.main()
