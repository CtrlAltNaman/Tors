# Local backend test harness

This is an ASR-free LAN test server for hardware bring-up. It acknowledges the
ESP32 hello message, accepts metrics and PCM frames, saves completed streams as
16 kHz mono WAV files, and can send a test tone or validated WAV clip back to
the device.

It is not the production ASR backend and does not perform speech recognition.

## Run

From the repository root:

    python -m pip install -r local_backend/requirements.txt
    python local_backend/local_backend.py

Optional playback:

    python local_backend/local_backend.py --test-tone
    python local_backend/local_backend.py --play-wav local_backend/test_audio/SampleAudio_16k_mono.wav

## Tests

    python local_backend/tests/test_local_backend.py
    python local_backend/tests/test_local_playback.py

The prepared sample loopback test is optional and requires the local test WAV:

    python local_backend/tests/test_sample_playback.py
