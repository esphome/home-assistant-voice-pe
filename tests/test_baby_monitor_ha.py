"""Run with HA installed: authenticated WebSocket, real TCP source, cleanup."""

import asyncio
import importlib.util
import socket
import tempfile
import unittest
from types import MappingProxyType


@unittest.skipUnless(
    importlib.util.find_spec("homeassistant"),
    "Requires Home Assistant test environment",
)
class HomeAssistantPlaybackTests(unittest.IsolatedAsyncioTestCase):
    async def test_authenticated_playback(self):
        from aiohttp import ClientSession
        from homeassistant import loader
        from homeassistant.auth import auth_manager_from_config
        from homeassistant.config_entries import ConfigEntries, ConfigEntry
        from homeassistant.core import HomeAssistant
        from homeassistant.setup import async_setup_component

        from custom_components.voice_pe_monitor import (
            async_setup,
            async_setup_entry,
            async_unload_entry,
        )

        with tempfile.TemporaryDirectory() as directory:
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                http_port = reservation.getsockname()[1]
            hass = HomeAssistant(directory)
            loader.async_setup(hass)
            hass.auth = await auth_manager_from_config(hass, [], [])
            hass.config_entries = ConfigEntries(hass, {})
            assert await async_setup_component(
                hass,
                "http",
                {"http": {"server_host": "127.0.0.1", "server_port": http_port}},
            )
            assert await async_setup_component(hass, "websocket_api", {})
            assert await async_setup(hass, {})
            entry = ConfigEntry(
                domain="voice_pe_monitor",
                title="test",
                data={"node_id": "voice", "upload_key": "k" * 40, "port": 0},
                options={},
                source="user",
                unique_id="voice",
                version=1,
                minor_version=1,
                discovery_keys=MappingProxyType({}),
            )
            assert await async_setup_entry(hass, entry)
            await hass.http.start()
            port = hass.http.site._server.sockets[0].getsockname()[1]
            user = await hass.auth.async_create_user("listener")
            refresh = await hass.auth.async_create_refresh_token(
                user, client_id="http://localhost/"
            )
            token = hass.auth.async_create_access_token(refresh)
            try:
                async with ClientSession() as client:
                    async with client.ws_connect(
                        f"http://127.0.0.1:{port}/api/websocket"
                    ) as ws:
                        assert (await ws.receive_json())["type"] == "auth_required"
                        await ws.send_json({"type": "auth", "access_token": "invalid"})
                        assert (await ws.receive_json())["type"] == "auth_invalid"
                    async with client.ws_connect(
                        f"http://127.0.0.1:{port}/api/websocket"
                    ) as ws:
                        await ws.receive_json()
                        await ws.send_json({"type": "auth", "access_token": token})
                        assert (await ws.receive_json())["type"] == "auth_ok"
                        await ws.send_json(
                            {
                                "id": 1,
                                "type": "voice_pe_monitor/listen",
                                "node_id": "voice",
                            }
                        )
                        result = await ws.receive_json()
                        assert result["success"], result
                        offline = await ws.receive_json()
                        assert offline["event"] == {
                            "type": "status",
                            "connected": False,
                        }, offline
                        receiver = hass.data["voice_pe_monitor"]["voice"]
                        audioport = receiver.server.sockets[0].getsockname()[1]
                        reader, writer = await asyncio.open_connection(
                            "127.0.0.1", audioport
                        )
                        writer.write(b"voice " + b"k" * 40 + b"\n")
                        await writer.drain()
                        assert await reader.readline() == b"OK\n"
                        online = await ws.receive_json()
                        assert online["event"]["connected"] is True, online
                        writer.write(b"\x01\x02" * 640)
                        await writer.drain()
                        audio = await ws.receive_json()
                        assert (
                            audio["event"]["type"] == "audio"
                            and audio["event"]["rate"] == 16000
                        ), audio
                        writer.close()
                        await writer.wait_closed()
                        assert (await ws.receive_json())["event"]["connected"] is False
                        await ws.send_json(
                            {"id": 2, "type": "unsubscribe_events", "subscription": 1}
                        )
                        assert (await ws.receive_json())["success"]
                        assert not receiver.listeners
                        print(
                            "LIVE HA: unauthenticated rejected, logged-in listener received PCM, disconnect/unsubscribe cleaned up"
                        )
            finally:
                await async_unload_entry(hass, entry)
                await hass.http.stop()
                await hass.async_stop()
