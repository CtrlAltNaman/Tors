import serial
import wave
import time
from datetime import datetime

PORT = "COM41"
BAUDRATE = 115200

SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2


timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
OUTPUT_FILE = f"recording_{timestamp}.wav"


print()
print("==============================")
print("ESP32-S3 AUDIO RECORDER")
print("==============================")
print()

print(f"Opening {PORT}...")

ser = serial.Serial(
    PORT,
    BAUDRATE,
    timeout=1
)

time.sleep(2)

print("Connected.")
print("Waiting for START...")
print("Press the ESP32 button to record.")
print()


# Clear anything that was printed before Python started
ser.reset_input_buffer()


# Wait for START
while True:

    line = ser.readline()

    if line:
        print("ESP32:", line.decode(errors="ignore").strip())

        if b"START" in line:
            break


print()
print("RECORDING...")
print()


audio_data = bytearray()

while True:

    # Look for STOP message.
    #
    # We cannot use readline() here because the audio
    # itself is binary data.

    data = ser.read(4096)

    if not data:
        continue

    audio_data.extend(data)

    # Check whether STOP appears at the end.
    #
    # STOP is ASCII and very unlikely to occur naturally,
    # but we'll handle it simply.

    stop_index = audio_data.find(b"STOP")

    if stop_index != -1:

        audio_data = audio_data[:stop_index]

        break


# Save WAV
with wave.open(OUTPUT_FILE, "wb") as wav:

    wav.setnchannels(CHANNELS)
    wav.setsampwidth(SAMPLE_WIDTH)
    wav.setframerate(SAMPLE_RATE)

    wav.writeframes(audio_data)


ser.close()


duration = len(audio_data) / (
    SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH
)


print()
print("==============================")
print("RECORDING FINISHED")
print("==============================")
print(f"File: {OUTPUT_FILE}")
print(f"Size: {len(audio_data)} bytes")
print(f"Duration: {duration:.2f} seconds")
print()