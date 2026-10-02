from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_firmware_targets_16mb_flash():
    sdkconfig = (ROOT / "sdkconfig").read_text()
    assert "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y" in sdkconfig
    assert 'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"' in sdkconfig
    assert "CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y" not in sdkconfig


def test_application_installs_usb_recorder_driver_before_audio_output():
    source = (ROOT / "main" / "main.c").read_text()
    assert "usb_serial_jtag_driver_install" in source
    assert "#define USB_TX_BUFFER_SIZE 4096" in source
    assert source.index("usb_serial_jtag_driver_install") < source.index("usb_serial_jtag_write_bytes")
    assert "uart_write_bytes" not in source


def test_usb_protocol_matches_record_script():
    source = (ROOT / "main" / "main.c").read_text()
    assert '#include "driver/usb_serial_jtag.h"' in source
    assert 'send_recorder_text("READY\\n")' in source
    assert 'send_recorder_text("START\\n")' in source
    assert 'send_recorder_text("STOP\\n")' in source


def test_main_component_declares_usb_serial_jtag_dependency():
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text()
    assert "esp_driver_usb_serial_jtag" in cmake


def test_i2s_mono_capture_selects_the_microphone_slot():
    source = (ROOT / "main" / "main.c").read_text()
    assert "rx_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT" in source


def test_boot_tone_uses_i2s_transmit_pin():
    source = (ROOT / "main" / "main.c").read_text()
    assert "#define I2S_AMP_DATA_PIN GPIO_NUM_8" in source
    assert "play_boot_audio" in source
    assert "i2s_channel_write" in source
    assert "i2s_channel_disable(i2s_tx_handle)" in source
    assert "boot_beeps" in source
    assert "Playing two startup beeps" in source


def test_startup_beeps_are_generated_in_firmware():
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text()
    source = (ROOT / "main" / "main.c").read_text()
    assert "EMBED_FILES" not in cmake
    assert "sinf(phase)" in source
    assert "MELODY_AMPLITUDE 7000.0f" in source
    assert "{880, 180}" in source


def test_runtime_buffers_leave_heap_for_wifi_and_websocket():
    source = (ROOT / "main" / "main.c").read_text()
    backend = (ROOT / "main" / "backend_client.c").read_text()

    assert "#define USB_TX_BUFFER_SIZE 4096" in source
    assert '"backend_init", 4096' in source
    playback = (ROOT / "main" / "playback_buffer.c").read_text()
    assert "playback_buffer_init()" in backend
    assert "xQueueCreateStatic(PLAYBACK_QUEUE_FRAMES" in playback
    assert "EXT_RAM_BSS_ATTR uint8_t playback_storage" in playback


def test_kws_inference_is_throttled_to_allow_wifi_tasks_to_run():
    source = (ROOT / "main" / "kws_detector.cc").read_text()

    assert "kws_inference_stride(invoke_budget_us, feature_budget_us," in source
    assert "portTICK_PERIOD_MS * 1000" in source
    assert "if (next > inference_stride) inference_stride = next;" in source


def test_main_loop_yields_to_wifi_and_watchdog():
    source = (ROOT / "main" / "main.c").read_text()

    assert "vTaskDelay(1);" in source
    assert "vTaskDelay(pdMS_TO_TICKS(1));" not in source


if __name__ == "__main__":
    test_firmware_targets_16mb_flash()
    test_application_installs_usb_recorder_driver_before_audio_output()
    test_usb_protocol_matches_record_script()
    test_main_component_declares_usb_serial_jtag_dependency()
    test_i2s_mono_capture_selects_the_microphone_slot()
    test_boot_tone_uses_i2s_transmit_pin()
    test_startup_beeps_are_generated_in_firmware()
    test_runtime_buffers_leave_heap_for_wifi_and_websocket()
    test_kws_inference_is_throttled_to_allow_wifi_tasks_to_run()
    test_main_loop_yields_to_wifi_and_watchdog()
    print("firmware settings tests passed")
