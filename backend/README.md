# Backend boundary

The production-oriented Python gateway is in backend/asr_gateway/. It owns the
frozen WebSocket protocol, stream reconstruction, simulation tools, and backend
tests.

The smaller firmware/esp32/local_backend.py remains as a convenient LAN
capture/playback harness for hardware bring-up. It is intentionally separate
from the production gateway so board debugging does not silently change the
evaluation server.

From backend/asr_gateway/:

    python -m venv .venv
    .venv\Scripts\Activate.ps1
    pip install -e ".[dev]"
    pytest
