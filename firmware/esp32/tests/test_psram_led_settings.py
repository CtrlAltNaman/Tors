"""Regression checks for the confirmed N16R8 board and physical LED wiring."""
from pathlib import Path
import ast
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BoardSettings(unittest.TestCase):
    longMessage = False

    def test_physical_led_mapping(self):
        source = (ROOT / 'main/device_leds.c').read_text()
        self.assertIn('#define RED_LED_PIN GPIO_NUM_15', source, 'red must be GPIO15')
        self.assertIn('#define GREEN_LED_PIN GPIO_NUM_7', source, 'green must be GPIO7')

    def test_octal_psram_enabled_with_internal_reserve(self):
        config = (ROOT / 'sdkconfig').read_text().splitlines()
        for setting in (
            'CONFIG_SPIRAM=y', 'CONFIG_SPIRAM_MODE_OCT=y',
            'CONFIG_SPIRAM_SPEED_80M=y', 'CONFIG_SPIRAM_BOOT_INIT=y',
            'CONFIG_SPIRAM_MEMTEST=y', 'CONFIG_SPIRAM_USE_MALLOC=y',
            'CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y',
            'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y',
            'CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768',
        ):
            with self.subTest(setting=setting):
                self.assertIn(setting, config, f'missing {setting}')

    def test_only_task_context_bulk_buffers_are_external(self):
        source = (ROOT / 'main/main.c').read_text()
        backend = (ROOT / 'main/backend_client.c').read_text()
        kws = (ROOT / 'main/kws_detector.cc').read_text()
        self.assertIn('static EXT_RAM_BSS_ATTR audio_history_t audio_history;', source)
        self.assertIn('static EXT_RAM_BSS_ATTR int16_t frame_samples[', backend)
        self.assertIn('static EXT_RAM_BSS_ATTR uint8_t audio_frame[', backend)
        self.assertNotIn('EXT_RAM_BSS_ATTR', kws)
        self.assertIn('static int32_t i2s_buffer[', source)
        self.assertIn('static int32_t audio_tx_buffer[', source)

    def test_psram_reported_separately_from_internal_heap(self):
        source = (ROOT / 'main/main.c').read_text()
        self.assertIn('esp_psram_get_size()', source)
        self.assertIn('MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT', source)
        self.assertIn('PSRAM total=', source)
        self.assertIn('psram_static_bss_bytes', source)
        self.assertIn('psram_heap_free_bytes', source)

    def test_status_summary_fits_local_log_snapshot(self):
        source = (ROOT / 'main/main.c').read_text()
        header = (ROOT / 'main/diagnostic_store.h').read_text()
        block = source.split('snprintf(metrics, sizeof(metrics),', 1)[1]
        block = block.split('(unsigned long long)', 1)[0]
        template = ''.join(ast.literal_eval(s) for s in
                           re.findall(r'"(?:\\.|[^"\\])*"', block))
        # Bound the real format using maximum counters, 32-char state labels,
        # and a conservative microphone level; catches clipping the HEALTH tail.
        def widest(match):
            spec = match.group()
            if spec.endswith('s'):
                return 'x' * 32
            if spec.endswith('f'):
                return '-32768.' + '0' * int(re.search(r'\.(\d+)', spec)[1])
            return '18446744073709551615' if 'll' in spec else '4294967295'
        rendered = re.sub(r'%(?:ll|l)?(?:\.\d+)?[usf]', widest, template)
        capacity = int(re.search(r'DIAGNOSTIC_SNAPSHOT_BYTES (\d+)', header)[1])
        self.assertLess(len(rendered), capacity, 'local snapshot truncates full status')


if __name__ == '__main__':
    unittest.main()
