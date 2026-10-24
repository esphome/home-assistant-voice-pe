"""Single-use, short-lived stream tickets for ESPHome speaker playback."""

import asyncio
import secrets
import struct
from time import monotonic

from aiohttp import web
from homeassistant.components.http import HomeAssistantView

from .receiver import RATE

# Open-ended PCM WAV; EOF ends the stream. 0xFFFFFFFF is conventional unknown length.
WAV_HEADER = struct.pack(
    "<4sI4s4sIHHIIHH4sI",
    b"RIFF",
    0xFFFFFFFF,
    b"WAVE",
    b"fmt ",
    16,
    1,
    1,
    RATE,
    RATE * 2,
    2,
    16,
    b"data",
    0xFFFFFFFF,
)


class PlaybackSessions:
    def __init__(self, stop_device):
        self.sessions = {}
        self.stop_device = stop_device

    def busy(self, device):
        return any(
            device in (item["source"], item["listener"])
            for item in self.sessions.values()
        )

    def create(self, source, listener, receiver):
        token = secrets.token_urlsafe(32)
        item = {
            "source": source,
            "listener": listener,
            "receiver": receiver,
            "expires": monotonic() + 15,
            "claimed": False,
            "queue": None,
            "ready": asyncio.get_running_loop().create_future(),
        }
        self.sessions[token] = item
        item["expiry_task"] = asyncio.create_task(self.expire(token))
        return token

    async def expire(self, token):
        await asyncio.sleep(15)
        if token in self.sessions and not self.sessions[token]["claimed"]:
            await self.close(token)

    async def close(self, token):
        item = self.sessions.pop(token, None)
        if item is None:
            return
        if not item["ready"].done():
            item["ready"].set_result(False)
        expiry = item["expiry_task"]
        if expiry is not asyncio.current_task():
            expiry.cancel()
        queue = item["queue"]
        if queue is not None:
            while not queue.empty():
                queue.get_nowait()
            queue.put_nowait(None)
        # Pop first so media disconnection cannot recursively stop the pair again.
        await asyncio.gather(
            self.stop_device(item["source"]),
            self.stop_device(item["listener"]),
            return_exceptions=True,
        )

    async def close_device(self, device):
        for token, item in tuple(self.sessions.items()):
            if device in (item["source"], item["listener"]):
                await self.close(token)

    async def stop(self):
        for token in tuple(self.sessions):
            await self.close(token)


class MonitorStreamView(HomeAssistantView):
    url = "/api/voice_pe_monitor/stream/{token}.wav"
    name = "api:voice_pe_monitor:stream"
    # ESPHome cannot send a HA user token. This URL is a single-use capability,
    # generated only by an authenticated HA service call, valid 15 s before claim.
    requires_auth = False

    async def get(self, request, token):
        sessions = request.app["hass"].data["voice_pe_monitor_sessions"]
        item = sessions.sessions.get(token)
        if item is None or item["claimed"] or monotonic() > item["expires"]:
            raise web.HTTPNotFound()
        receiver = item["receiver"]
        if not receiver.connected:
            await sessions.close(token)
            raise web.HTTPConflict(text="Source offline")
        item["claimed"] = True
        queue = item["queue"] = asyncio.Queue(maxsize=8)

        def receive(event):
            if event["type"] == "status":
                if event["connected"]:
                    return
                while not queue.empty():
                    queue.get_nowait()
                queue.put_nowait(None)
            elif not queue.full():
                queue.put_nowait(event["data"])
            else:
                # Slow speakers stop the pair, rather than accumulating old audio.
                while not queue.empty():
                    queue.get_nowait()
                queue.put_nowait(None)

        receiver.listeners.add(receive)
        response = web.StreamResponse(
            headers={
                "Content-Type": "audio/wav",
                "Cache-Control": "no-store",
                "X-Content-Type-Options": "nosniff",
            }
        )
        try:
            await response.prepare(request)
            if not item["ready"].done():
                item["ready"].set_result(True)
            await response.write(WAV_HEADER)
            while token in sessions.sessions:
                data = await asyncio.wait_for(queue.get(), 5)
                if data is None:
                    break
                await asyncio.wait_for(response.write(data), 2)
        except (TimeoutError, ConnectionError):
            pass
        finally:
            receiver.listeners.discard(receive)
            await sessions.close(token)
        return response
