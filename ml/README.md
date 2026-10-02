# Model pipeline boundary

This directory is the future home of dataset manifests, preprocessing, training,
evaluation, and model export scripts.

The current model work remains in firmware/esp32/ML MODEL SPECS and
firmware/esp32/NEW BETTER MODEL until it is extracted with hash-checked
artifacts. Raw recordings and generated datasets are ignored by the repository.

Every exported model should document:

- keyword and class index
- sample rate and feature configuration
- input/output tensor shapes and quantization
- model SHA-256
- true-positive and false-activation results
- license/provenance of training data and dependencies
