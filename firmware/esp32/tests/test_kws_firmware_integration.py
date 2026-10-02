from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def test_kws_model_asset_matches_exported_hello_tors_model():
    exported = ROOT / "ML MODEL SPECS" / "hello_tors_outputs" / "export" / "hello_tors_model_data.h"
    firmware_asset = ROOT / "main" / "kws_model_data.h"

    assert firmware_asset.exists()
    assert firmware_asset.read_bytes() == exported.read_bytes()
    package = ROOT / "NEW BETTER MODEL/hello_tors_esp32s3_deploy/deploy_esp32s3"
    embedded = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})\b", firmware_asset.read_text()))
    assert embedded == (package / "export/hello_tors_kws_int8.tflite").read_bytes()


def test_firmware_declares_kws_runtime_and_backend_transport():
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text()
    source = (ROOT / "main" / "main.c").read_text()
    detector_header = (ROOT / "main" / "kws_detector.h").read_text()

    assert '"kws_detector.cc"' in cmake
    assert '"backend_client.c"' in cmake
    assert "esp-tflite-micro" in cmake
    assert '#include "kws_detector.h"' in source
    assert '#include "backend_client.h"' in source
    assert "kws_detector_process" in detector_header
    assert "backend_client_start_stream" in source


def test_supplied_tables_vectors_and_feature_math_are_preserved():
    package = ROOT / "NEW BETTER MODEL/hello_tors_esp32s3_deploy/deploy_esp32s3/firmware/hello_tors_kws/main"
    for name in ("kws_feature_tables.h", "kws_test_vectors.h"):
        assert (ROOT / "main" / name).read_text() == (package / name).read_text()
    reference = (package / "kws_features.c").read_text()
    actual = (ROOT / "main/kws_features.c").read_text()
    # The only implementation difference is scratch lifetime, not arithmetic.
    actual = re.sub(r"    // Single owner:.*?    static float re", "    float re", actual, flags=re.S)
    assert actual == reference
    cmake = (ROOT / "main/CMakeLists.txt").read_text()
    assert "-ffp-contract=off" in cmake


def test_continuous_kws_uses_trained_feature_configuration():
    source = (ROOT / "main" / "kws_features.h").read_text()
    source += (ROOT / "main" / "kws_frontend.h").read_text()
    source = " ".join(source.split())
    assert "KWS_SAMPLE_RATE 16000" in source
    assert "KWS_FRAME_LEN 480" in source
    assert "KWS_FRAME_STEP 320" in source
    assert "KWS_NUM_MFCC 40" in source
    assert "KWS_NUM_FRAMES 49" in source
    assert "KWS_INPUT_SCALE 0.558301985" in source
    assert "KWS_INPUT_ZERO_POINT 93" in source


def test_kws_threshold_matches_user_confirmed_trial():
    source = (ROOT / "main" / "kws_frontend.h").read_text()

    assert "KWS_DETECTION_THRESHOLD 0.450f" in source
    assert "KWS_REQUIRED_POSITIVES 2" in source
    assert "KWS_REFRACTORY_US 1000000" in source


def test_kws_resolver_registers_model_shape_operator():
    source = (ROOT / "main" / "kws_detector.cc").read_text()

    assert "resolver.AddShape()" in source


def test_kws_resolver_registers_model_strided_slice_operator():
    source = (ROOT / "main" / "kws_detector.cc").read_text()

    assert "resolver.AddStridedSlice()" in source


def test_kws_resolver_registers_ds_cnn_operators():
    source = (ROOT / "main" / "kws_detector.cc").read_text()

    assert "MicroMutableOpResolver<9>" in source
    assert "resolver.AddConv2D()" in source
    assert "resolver.AddDepthwiseConv2D()" in source
    assert "resolver.AddMean()" in source
    assert "resolver.AddFullyConnected()" in source
    assert "resolver.AddSoftmax()" in source


def test_kws_resolver_registers_model_pack_operator():
    source = (ROOT / "main" / "kws_detector.cc").read_text()

    assert "resolver.AddPack()" in source


def test_kws_tensor_arena_covers_model_allocation():
    source = (ROOT / "main" / "kws_detector.cc").read_text()

    assert "KWS_TENSOR_ARENA_BYTES (112 * 1024)" in source
    assert "interpreter->AllocateTensors() != kTfLiteOk" in source
    assert 'selftest("positive"' in source
    assert 'selftest("negative"' in source
    assert "if (!pos_ok || !neg_ok) return false;" in source
    assert "valid_tensor_contract()" in source


def test_metrics_and_audio_playback_contract_are_represented_in_firmware():
    source = (ROOT / "main" / "main.c").read_text()
    backend = (ROOT / "main" / "backend_client.c").read_text()

    assert "metrics" in source
    assert "model_flash_bytes" in source
    assert "play_start" in backend
    assert "i2s_channel_write" in source


if __name__ == "__main__":
    test_kws_model_asset_matches_exported_hello_tors_model()
    test_firmware_declares_kws_runtime_and_backend_transport()
    test_supplied_tables_vectors_and_feature_math_are_preserved()
    test_continuous_kws_uses_trained_feature_configuration()
    test_kws_threshold_matches_user_confirmed_trial()
    test_kws_resolver_registers_model_shape_operator()
    test_kws_resolver_registers_model_strided_slice_operator()
    test_kws_resolver_registers_ds_cnn_operators()
    test_kws_resolver_registers_model_pack_operator()
    test_kws_tensor_arena_covers_model_allocation()
    test_metrics_and_audio_playback_contract_are_represented_in_firmware()
    print("KWS firmware integration tests passed")
