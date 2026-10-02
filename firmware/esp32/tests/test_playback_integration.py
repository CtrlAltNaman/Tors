from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PlaybackIntegration(unittest.TestCase):
    def test_speaker_volume_applies_to_backend_audio_and_boot_beeps(self):
        header = (ROOT / 'main/audio_conversion.h').read_text()
        source = (ROOT / 'main/main.c').read_text()
        self.assertIn('#define SPEAKER_VOLUME_PERCENT 70', header)
        self.assertIn('tx_samples[i] = speaker_pcm_to_i2s(frame.samples[i]);', source)
        self.assertIn('audio_tx_buffer[i] = speaker_pcm_to_i2s((int16_t)sample);', source)

    def test_bounded_psram_buffer_replaces_four_frame_drop_queue(self):
        self.assertTrue((ROOT / 'main/playback_buffer.c').exists(), 'missing playback buffer')
        source = (ROOT / 'main/playback_buffer.c').read_text()
        header = (ROOT / 'main/playback_buffer.h').read_text()
        self.assertTrue('PLAYBACK_QUEUE_FRAMES 64' in header)
        self.assertIn('PLAYBACK_PREBUFFER_FRAMES 5', header)
        self.assertTrue('EXT_RAM_BSS_ATTR' in source)
        self.assertTrue('xQueueCreateStatic' in source)
        self.assertTrue('pdMS_TO_TICKS(40)' in source)

    def test_startup_prebuffer_and_receive_write_timing(self):
        source = (ROOT / 'main/main.c').read_text()
        worker = source.split('static void speaker_playback_task', 1)[1].split('static esp_err_t play_boot_audio', 1)[0]
        self.assertIn('playback_buffer_ready(frame.generation, 1)', worker)
        self.assertIn('PLAYBACK_PREBUFFER_WAIT_MS', worker)
        self.assertIn('playback_buffer_write_timing', worker)
        self.assertLess(worker.index('playback_buffer_ready'), worker.index('i2s_channel_enable'))
        self.assertIn('rx_gap_max=', source)
        self.assertIn('write_max=', source)
        self.assertIn('playback_rx_gap_max_ms', source)
        self.assertIn('playback_write_max_ms', source)

    def test_dma_silence_and_tail_drain(self):
        source = (ROOT / 'main/main.c').read_text()
        self.assertTrue('channel_config.auto_clear_after_cb = true;' in source)
        self.assertTrue('SPEAKER_DRAIN_MS' in source)
        self.assertTrue('playback_buffer_current' in source)

    def test_no_per_frame_warning_and_summary_has_playback_counters(self):
        backend = (ROOT / 'main/backend_client.c').read_text()
        source = (ROOT / 'main/main.c').read_text()
        self.assertTrue('Speaker playback queue full; dropping frame' not in backend)
        self.assertTrue('PLAY rx=' in source)
        self.assertTrue('playback_overflows' in source)

    def test_acknowledgement_cannot_block_speaker_task(self):
        source = (ROOT / 'main/main.c').read_text()
        worker = source.split('static void speaker_playback_task', 1)[1].split('static esp_err_t play_boot_audio', 1)[0]
        self.assertTrue('send_text' not in worker and 'send_playback_stop' not in worker)
        self.assertTrue('backend_client_queue_playback_stop' in worker)
        self.assertTrue('backend_client_service_playback();' in source)
        last = worker.split('if (frame.last)', 1)[1]
        self.assertLess(last.index('SPEAKER_DRAIN_MS'), last.index('backend_client_queue_playback_stop'))

if __name__ == '__main__':
    unittest.main()
