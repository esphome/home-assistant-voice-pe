"""Bounded, memory-only PCM receiver. Independent of Home Assistant for testing."""

import asyncio
import hmac

RATE = 16000
CHUNK = 1280  # 40 ms of mono signed little-endian 16-bit PCM.


class Receiver:
    def __init__(self, node_id, key):
        self.node_id = node_id
        self._key = key
        self.connected = False
        self.online = asyncio.Event()
        self.listeners = set()
        self.server = None
        self._writer = None
        self._clients = set()
        self._tasks = set()

    def provisioning_data(self):
        """Internal API payload, never returned by the browser websocket commands."""
        return {"key": self._key}

    async def start(self, host, port):
        self.server = await asyncio.start_server(self.handle, host, port, limit=256)

    def _publish(self, event):
        for listener in tuple(self.listeners):
            try:
                listener(event)
            except (ConnectionError, RuntimeError):
                # A closed/broken browser must not tear down the audio source.
                self.listeners.discard(listener)

    def _status(self, connected):
        self.connected = connected
        if connected:
            self.online.set()
        else:
            self.online.clear()
        self._publish({"type": "status", "connected": connected})

    async def handle(self, reader, writer):
        # Limit even unauthenticated connections and their handshake memory/time.
        if len(self._clients) >= 8:
            writer.close()
            return
        self._clients.add(writer)
        task = asyncio.current_task()
        self._tasks.add(task)
        accepted = False
        try:
            hello = await asyncio.wait_for(reader.readline(), 5)
            parts = hello.rstrip(b"\n").split(b" ")
            if (
                len(parts) != 2
                or parts[0] != self.node_id.encode()
                or not hmac.compare_digest(parts[1], self._key.encode())
            ):
                return
            # Do not let a duplicate uplink displace an active source.
            if self._writer is not None:
                return
            self._writer = writer
            accepted = True
            writer.write(b"OK\n")
            await writer.drain()
            self._status(True)
            while True:
                data = await asyncio.wait_for(reader.readexactly(CHUNK), 5)
                self._publish({"type": "audio", "data": data})
        except (TimeoutError, asyncio.IncompleteReadError, ConnectionError, ValueError):
            pass
        finally:
            if accepted:
                self._writer = None
                self._status(False)
            self._clients.discard(writer)
            self._tasks.discard(task)
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass

    async def stop(self):
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        for writer in tuple(self._clients):
            writer.close()
        tasks = tuple(self._tasks)
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        self.listeners.clear()
