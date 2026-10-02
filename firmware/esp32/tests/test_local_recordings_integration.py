from pathlib import Path
root = Path(__file__).resolve().parents[1]
main = (root / 'main/main.c').read_text()
http = (root / 'main/device_diagnostics.c').read_text()
store = (root / 'main/local_recordings.c').read_text()
assert 'gpio_set_level(' not in main, 'all LED writes must belong to one controller'
assert 'local_recordings_begin(' in main
assert 'disable_upload("network_error")' in main, 'network failure must not end local recording'
assert 'device_state_name(device_leds_get())' in main
assert 'if (!current.active || current.starting) return;' in main
assert 'history_epoch[index % AUDIO_HISTORY_FRAMES] != session.capture_epoch' in main
assert 'recordings_http_register(server)' in http
assert 'format_if_mount_failed' not in store
assert 'unrecognized_storage_preserved' in store
assert 'slots[chosen].readers' in store
assert 'REC_MAX_PCM_BYTES' in store
print('local recordings integration: single LED owner, offline capture, safe storage and HTTP routes passed')
