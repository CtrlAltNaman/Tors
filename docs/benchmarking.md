# Benchmarking and evidence

The SIH evaluation requires full-system evidence. Model size alone is not a RAM
result, and successful compilation alone is not an accuracy result.

## Required measurements

| Metric | Measurement | Required context |
| --- | --- | --- |
| Flash | final partition/build artifact sizes | ESP-IDF version, target, config |
| RAM | static data/BSS, heap high-water, tensor arena, task stacks | internal RAM and PSRAM separately |
| Idle CPU | capture + frontend + inference over a fixed interval | board frequency, evaluation stride, method |
| True-positive rate | labeled keyword trials | speaker, distance, room, microphone, threshold |
| False activations | false events per hour | silence/noise mix and total test duration |
| Wake latency | keyword end to first backend audio receipt | clock-sync method and p50/p95/p99 |

## Evidence record

Each benchmark report should include:

    firmware_commit:
    model_sha256:
    board:
    microphone:
    esp_idf_version:
    cpu_frequency:
    flash_configuration:
    psram_configuration:
    keyword_threshold:
    decision_rule:
    environment:
    sample_count_or_duration:
    measurement_method:
    results:
    limitations:

## Acceptance gates

Do not mark a gate as passed until the raw logs or report are committed:

- [ ] Device boots and model self-tests pass.
- [ ] Full firmware fits the intended RAM budget under real runtime load.
- [ ] Idle CPU is measured continuously, not inferred from one inference time.
- [ ] Accuracy is measured on held-out speakers and environments.
- [ ] False activations are measured over a stated number of hours.
- [ ] Backend receipt timestamps are recorded for latency.
- [ ] Model and firmware hashes are captured with the report.

The current firmware review documents known limitations. Read it before using
any existing number in a submission:
../firmware/esp32/docs/ESP32S3_ML_DEPLOYMENT_REVIEW.md.
