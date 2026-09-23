"""Generate audio samples for README voice comparison."""
import struct
import wave
import urllib.request
import json
import os

VOICES_KOKORO = [
    "af_heart",
    "af_bella",
    "af_nicole",
    "am_adam",
    "am_michael",
    "bf_emma",
    "bm_george",
    "ef_dora",
    "em_alex",
    "em_santa",
]

VOICES_PIPER = [
    "es_MX-claude-high",
]

SAMPLE_TEXT = (
    "The old bookshop on the corner had a peculiar charm about it. "
    "Dust motes danced in the sunlight that streamed through tall windows, "
    "and the smell of aged paper filled every room. "
    "It was the kind of place where you could lose an entire afternoon "
    "without even noticing."
)

SAMPLE_TEXT_ES = (
    "En un futuro desolado, miles de años después de que una invasión alienígena obligara a la humanidad a abandonar la Tierra y refugiarse en la Luna."
    "Para recuperar su hogar, los humanos crearon el proyecto YoRHa,"
    "un ejército de androides de combate altamente avanzados que libran una guerra subsidiaria interminable"
    "contra las formas de vida mecánicas creadas por los invasores."
)

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "..", "docs", "audio")
os.makedirs(OUTPUT_DIR, exist_ok=True)

SAMPLE_RATE = 24000
SAMPLE_WIDTH = 2  # 16-bit
CHANNELS = 1

# Check with a request to /v1/health which model is loaded
req = urllib.request.Request(
    "http://127.0.0.1:7860/v1/health",
    headers={"Content-Type": "application/json"},
)

try:
    with urllib.request.urlopen(req, timeout=60) as resp:
        health = resp.read()
        
    if (b'Kokoro' in health): VOICES = VOICES_KOKORO
    elif (b'Piper' in health): VOICES = VOICES_PIPER
    else: raise Exception("Model not recognized")
except Exception as e:
        print(f"  ERROR reaching the API: {e}")

for voice in VOICES:
    print(f"Generating sample for {voice}...")
    payload = json.dumps({
        "text": SAMPLE_TEXT if voice[0] != 'e' else SAMPLE_TEXT_ES,
        "voice": voice,
        "speed": 1.0,
        "format": "pcm_24k_16bit",
    }).encode()

    req = urllib.request.Request(
        "http://127.0.0.1:7860/v1/tts",
        data=payload,
        headers={"Content-Type": "application/json"},
    )

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            pcm_data = resp.read()

        wav_path = os.path.join(OUTPUT_DIR, f"{voice}.wav")
        with wave.open(wav_path, "wb") as wf:
            wf.setnchannels(CHANNELS)
            wf.setsampwidth(SAMPLE_WIDTH)
            wf.setframerate(SAMPLE_RATE)
            wf.writeframes(pcm_data)

        size_kb = os.path.getsize(wav_path) / 1024
        duration = len(pcm_data) / (SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS)
        print(f"  -> {wav_path} ({size_kb:.0f} KB, {duration:.1f}s)")
    except Exception as e:
        print(f"  ERROR for {voice}: {e}")

print("\nDone!")
