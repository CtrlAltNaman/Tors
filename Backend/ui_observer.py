"""Lightweight read-only WebSocket observer for the REVENANT UI dashboard.

Runs on a separate port (default 8766) and broadcasts pipeline state events
to connected browser clients.  Does NOT modify the audio pipeline, ASR,
LLM, TTS, or any existing backend behaviour.

Usage — imported and started by the VoiceServer; zero config needed.
"""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Any, Optional, Set

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

LOG = logging.getLogger(__name__)

# ── Event types sent to UI ──────────────────────────────────────────────
# {type: "state",     state: "idle"|"wake_detected"|"listening"|"processing"|"speaking"}
# {type: "question",  text: "..."}      ← the user's post-wake-word question
# {type: "answer",    text: "..."}      ← the assistant's LLM response
# {type: "connected", state: "idle"}    ← sent on initial UI connection


class UIObserver:
    """Broadcasts JSON events to all connected UI WebSocket clients."""

    def __init__(self) -> None:
        self._clients: Set[ServerConnection] = set()
        self._state: str = "idle"
        self._serve_task: Optional[asyncio.Task] = None

    # ── Public API (called by VoiceServer) ──────────────────────────────

    async def broadcast(self, msg: dict[str, Any]) -> None:
        """Send a JSON message to every connected UI client."""
        if not self._clients:
            return
        payload = json.dumps(msg)
        dead: list[ServerConnection] = []
        for ws in self._clients:
            try:
                await ws.send(payload)
            except (ConnectionClosed, Exception):
                dead.append(ws)
        for ws in dead:
            self._clients.discard(ws)

    async def set_state(self, state: str) -> None:
        """Update pipeline state and notify all UI clients."""
        self._state = state
        await self.broadcast({"type": "state", "state": state})

    async def send_question(self, text: str) -> None:
        """Send the user's question (post wake-word) to UI clients."""
        if text and text.strip():
            await self.broadcast({"type": "question", "text": text.strip()})

    async def send_answer(self, text: str) -> None:
        """Send the assistant's response to UI clients."""
        if text and text.strip():
            await self.broadcast({"type": "answer", "text": text.strip()})

    # ── WebSocket server ────────────────────────────────────────────────

    async def _handler(self, ws: ServerConnection) -> None:
        """Handle a UI client connection."""
        self._clients.add(ws)
        LOG.info("UI client connected (%d total)", len(self._clients))
        try:
            # Send current state on connect
            await ws.send(json.dumps({
                "type": "connected",
                "state": self._state,
            }))
            # Keep connection alive — UI is read-only, we just wait for close
            async for _ in ws:
                pass  # Ignore any messages from UI
        except ConnectionClosed:
            pass
        finally:
            self._clients.discard(ws)
            LOG.info("UI client disconnected (%d remaining)", len(self._clients))

    async def start(self, host: str = "0.0.0.0", port: int = 8766) -> None:
        """Start the UI observer WebSocket server."""
        try:
            server = await serve(
                self._handler,
                host,
                port,
                ping_interval=20,
                ping_timeout=20,
            )
            LOG.info("UI observer listening on ws://%s:%d/ui", host, port)
            # Keep server running (it will be cancelled when the main loop stops)
            await asyncio.Future()
        except Exception as e:
            LOG.warning("UI observer failed to start: %s", e)

    def start_background(self, host: str = "0.0.0.0", port: int = 8766) -> None:
        """Start the observer server as a background asyncio task."""
        self._serve_task = asyncio.ensure_future(self.start(host, port))
