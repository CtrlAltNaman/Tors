# Host simulator boundary

The simulator will replay labeled audio through the same feature and decision
policy used by the ESP32, then exercise the backend transport without hardware.

It should report detection timing, missed detections, false activations, packet
loss, and first-frame receipt latency. Do not use simulator results as a
substitute for measurements on the target ESP32-S3.
