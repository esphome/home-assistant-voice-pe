"""Real HA services + HTTP WAV + TCP PCM, including either end stopping the pair."""

import asyncio
import importlib.util
import socket
import tempfile
import unittest
from types import MappingProxyType


@unittest.skipUnless(
    importlib.util.find_spec("homeassistant"), "Requires HA environment"
)
class PairPlaybackTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from aiohttp import ClientSession
        from homeassistant import loader
        from homeassistant.auth import auth_manager_from_config
        from homeassistant.config_entries import ConfigEntries, ConfigEntry
        from homeassistant.core import HomeAssistant
        from homeassistant.helpers import device_registry as dr
        from homeassistant.helpers import entity_registry as er
        from homeassistant.setup import async_setup_component

        from custom_components.voice_pe_monitor import async_setup, async_setup_entry

        self.temp = tempfile.TemporaryDirectory()
        self.hass = hass = HomeAssistant(self.temp.name)
        loader.async_setup(hass)
        hass.auth = await auth_manager_from_config(hass, [], [])
        hass.config_entries = ConfigEntries(hass, {})
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        self.base = f"http://127.0.0.1:{port}"
        assert await async_setup_component(
            hass, "http", {"http": {"server_host": "127.0.0.1", "server_port": port}}
        )
        assert await async_setup_component(hass, "websocket_api", {})
        await async_setup(hass, {})
        self.entry = ConfigEntry(
            domain="voice_pe_monitor",
            title="voice",
            data={"node_id": "voice", "upload_key": "k" * 40, "port": 0},
            options={},
            source="user",
            unique_id="voice",
            version=1,
            minor_version=1,
            discovery_keys=MappingProxyType({}),
        )
        await async_setup_entry(hass, self.entry)
        self.receiver = hass.data["voice_pe_monitor"]["voice"]
        await dr.async_get(hass).async_load()
        self.devices = []
        for name in ("voice", "listener"):
            entry = ConfigEntry(
                domain="esphome",
                title=name,
                data={"device_name": name},
                options={},
                source="user",
                unique_id=name,
                version=1,
                minor_version=1,
                discovery_keys=MappingProxyType({}),
            )
            hass.config_entries._entries[entry.entry_id] = entry
            device = dr.async_get(hass).async_get_or_create(
                config_entry_id=entry.entry_id, identifiers={("esphome", name)}
            )
            self.devices.append(device.id)
        registry = er.async_get(hass)
        await registry.async_load()
        entity = registry.async_get_or_create(
            "switch",
            "esphome",
            "listener-monitor-switch",
            device_id=self.devices[1],
            config_entry=entry,
            original_name="Радионяня: слушатель",
        )
        self.listener_switch = entity.entity_id
        self.writer = None
        self.response = None
        self.connect_listener = True
        self.pump = None
        self.urls = asyncio.Queue()
        self.stops = []
        self.both_stopped = asyncio.Event()
        self.pcm = b"\x01\x02" * 640
        self.received_keys = []

        async def configure(call):
            self.received_keys.append(call.data["key"])

        async def source(call):
            assert self.received_keys and self.received_keys[-1] == "k" * 40
            port = self.receiver.server.sockets[0].getsockname()[1]
            reader, self.writer = await asyncio.open_connection("127.0.0.1", port)
            self.writer.write(b"voice " + b"k" * 40 + b"\n")
            await self.writer.drain()
            assert await reader.readline() == b"OK\n"

            async def pump():
                try:
                    while not self.writer.is_closing():
                        self.writer.write(self.pcm)
                        await self.writer.drain()
                        await asyncio.sleep(0.04)
                except ConnectionError:
                    pass

            self.pump = asyncio.create_task(pump())

        async def listen(call):
            hass.states.async_set(self.listener_switch, "on")
            self.urls.put_nowait(call.data["url"])
            if self.connect_listener:
                self.response = await self.client.get(call.data["url"])

        async def stop_source(call):
            self.stops.append("voice")
            if self.writer:
                self.writer.close()
            if set(self.stops) == {"voice", "listener"}:
                self.both_stopped.set()

        async def stop_listener(call):
            self.stops.append("listener")
            if self.response:
                self.response.close()
            if set(self.stops) == {"voice", "listener"}:
                self.both_stopped.set()

        hass.services.async_register("esphome", "voice_monitor_configure", configure)
        hass.services.async_register("esphome", "voice_monitor_source", source)
        hass.services.async_register("esphome", "voice_monitor_stop", stop_source)
        hass.services.async_register("esphome", "listener_monitor_listen", listen)
        hass.services.async_register("esphome", "listener_monitor_stop", stop_listener)
        await hass.http.start()
        self.client = ClientSession()

    async def asyncTearDown(self):
        from custom_components.voice_pe_monitor import async_unload_entry

        await self.hass.data["voice_pe_monitor_sessions"].stop()
        if self.pump:
            self.pump.cancel()
            await asyncio.gather(self.pump, return_exceptions=True)
        await self.client.close()
        await async_unload_entry(self.hass, self.entry)
        await self.hass.http.stop()
        await self.hass.async_stop()
        self.temp.cleanup()

    async def start_pair(self):
        from custom_components.voice_pe_monitor.playback import WAV_HEADER

        await self.hass.services.async_call(
            "voice_pe_monitor",
            "start_source",
            {"device_id": self.devices[0]},
            blocking=True,
        )
        await self.hass.services.async_call(
            "voice_pe_monitor",
            "start_listener",
            {
                "source_device_id": self.devices[0],
                "device_id": self.devices[1],
                "ha_url": self.base,
            },
            blocking=True,
        )
        self.url = await self.urls.get()
        self.assertEqual(self.response.status, 200)
        self.assertEqual(await self.response.content.readexactly(44), WAV_HEADER)
        self.assertEqual(await self.response.content.readexactly(1280), self.pcm)
        async with self.client.get(self.url) as replay:
            self.assertEqual(
                replay.status, 404
            )  # Ticket cannot be reused, even on another browser.

    async def test_source_button_stops_both(self):
        await self.start_pair()
        self.writer.close()  # Source firmware button closes its TCP uplink.
        await asyncio.wait_for(self.both_stopped.wait(), 2)
        self.assertCountEqual(self.stops, ["voice", "listener"])
        self.assertFalse(self.hass.data["voice_pe_monitor_sessions"].sessions)

    async def test_listener_button_stops_both(self):
        await self.start_pair()
        self.response.close()  # Listener firmware button stops its HTTP decoder.
        await asyncio.wait_for(self.both_stopped.wait(), 2)
        self.assertCountEqual(self.stops, ["voice", "listener"])
        self.assertFalse(self.hass.data["voice_pe_monitor_sessions"].sessions)

    async def test_ha_stop_by_either_device_stops_both(self):
        await self.start_pair()
        await self.hass.services.async_call(
            "voice_pe_monitor", "stop", {"device_id": self.devices[1]}, blocking=True
        )
        self.assertCountEqual(self.stops, ["voice", "listener"])
        self.assertFalse(self.hass.data["voice_pe_monitor_sessions"].sessions)

    async def test_button_during_startup_stops_pair_before_http_connects(self):
        from homeassistant.exceptions import HomeAssistantError

        self.connect_listener = False
        await self.hass.services.async_call(
            "voice_pe_monitor",
            "start_source",
            {"device_id": self.devices[0]},
            blocking=True,
        )
        task = asyncio.create_task(
            self.hass.services.async_call(
                "voice_pe_monitor",
                "start_listener",
                {
                    "device_id": self.devices[1],
                    "source_device_id": self.devices[0],
                    "ha_url": self.base,
                },
                blocking=True,
            )
        )
        await asyncio.wait_for(self.urls.get(), 2)
        self.hass.states.async_set(self.listener_switch, "off")
        await asyncio.wait_for(self.both_stopped.wait(), 2)
        with self.assertRaises(HomeAssistantError):
            await task
        self.assertCountEqual(self.stops, ["voice", "listener"])
        self.assertFalse(self.hass.data["voice_pe_monitor_sessions"].sessions)

    async def test_form_has_no_key_and_generates_one_server_side(self):
        from custom_components.voice_pe_monitor.config_flow import MonitorConfigFlow

        flow = MonitorConfigFlow()
        flow.hass = self.hass
        flow.context = {"source": "user"}
        flow.flow_id = "test-flow"
        flow.handler = "voice_pe_monitor"
        form = await flow.async_step_user()
        assert "upload_key" not in {str(key) for key in form["data_schema"].schema}
        result = await flow.async_step_user({"node_id": "another-source", "port": 8125})
        self.assertEqual(result["type"], "create_entry")
        self.assertGreaterEqual(len(result["data"]["upload_key"]), 32)

    async def test_api_registration_and_switch_on_provision_without_scripts(self):
        await asyncio.sleep(0.01)  # Drain the service-registered callback from setup.
        self.assertIn("k" * 40, self.received_keys)
        self.received_keys.clear()
        from homeassistant.helpers import device_registry as dr
        from homeassistant.helpers import entity_registry as er

        device = dr.async_get(self.hass).async_get(self.devices[0])
        entry = self.hass.config_entries.async_get_entry(
            next(iter(device.config_entries))
        )
        entity = er.async_get(self.hass).async_get_or_create(
            "switch",
            "esphome",
            "voice-monitor-mode",
            device_id=self.devices[0],
            config_entry=entry,
            original_name="Радионяня",
        )
        self.hass.states.async_set(entity.entity_id, "on")
        await asyncio.sleep(0.01)
        self.assertEqual(self.received_keys, ["k" * 40])
