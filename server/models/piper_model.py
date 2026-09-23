# ============================================================================
# VoiceLink — Piper TTS Model Backend
# ============================================================================
#
# HOW THIS DIFFERS FROM KOKORO:
# Kokoro is one shared pipeline that knows many voices. Piper is the
# opposite: each voice is its OWN pair of files on disk —
#   <voice_id>.onnx       the VITS acoustic model + vocoder, as ONNX
#   <voice_id>.onnx.json  config: phoneme map, sample rate, espeak language
#
# So there's no single "Piper model" to load at startup. Instead we lazily
# load one PiperVoice object per voice id, the first time it's actually
# requested, and cache it — same idea as Kokoro's per-language pipeline
# cache in kokoro_model.py, just keyed by voice instead of by language.
#
# SAMPLE RATE — READ THIS BEFORE CHANGING ANYTHING HERE:
# Piper voices are natively 22050Hz (a few are 16000Hz). Everything
# downstream of this file — tts_engine.cpp's GetOutputFormat(), the
# /v1/tts response headers, research/03_test_server.py's WAV writer —
# hardcodes 24kHz and does NOT read VoiceInfo.sample_rate. So we resample
# every chunk to settings.audio.sample_rate (24000) before yielding it.
# Skip this and audio plays back pitch-shifted/too fast, silently, because
# nothing downstream double-checks the real rate.
# ============================================================================

from math import gcd
from pathlib import Path
from typing import Generator, TYPE_CHECKING

import numpy as np
from loguru import logger

from server.config import settings
from server.models.base import TTSModel, VoiceInfo

import os

if TYPE_CHECKING:
    from piper import PiperVoice

# --- Piper Voice Catalog ---
# Each id must match a real voice from https://huggingface.co/rhasspy/piper-voices
PIPER_VOICES: list[VoiceInfo] = [
    # English
    
    # Spanish
    VoiceInfo(
        id="es_ES-davefx-medium",
        name="Davefx",
        language="es-ES",
        gender="male",
        description="Spanish (Spain) voice, fine-tuned from the English Lessac voice.",
        model="piper",
        tags=["spanish", "piper"],
        sample_rate=22050,
    ),
    VoiceInfo(
        id="es_ES-sharvard-medium",
        name="Sharvard",
        language="es-ES",
        gender="male",
        description="Spanish (Spain) voice.",
        model="piper",
        tags=["spanish", "piper"],
        sample_rate=22050,
    ),
    VoiceInfo(
        id="es_MX-claude-high",
        name="Claude",
        language="es-MX",
        gender="female",
        description="Spanish (Mexico) voice, high quality.",
        model="piper",
        tags=["spanish", "mexico", "piper"],
        sample_rate=22050,
    ),
]


class PiperModel(TTSModel):
    """
    Piper TTS backend.
        
    Unlike KokoroModel's single shared KPipeline, each Piper voice is its
    own model file and is loaded independently — see _get_voice().
    """
    
    def __init__(self, voices_dir: Path | None = None, use_cuda: bool = False):
        self._voices_dir = Path(voices_dir) if voices_dir else settings.model.piper_voices_dir
        self._use_cuda = use_cuda
        self._voices: dict[str, "PiperVoice"] = {}
        self._loaded = False

    def load(self) -> None:
        """
        Piper has no shared model to load up front — individual voices
        load lazily on first use (see _get_voice). This just marks the
        backend ready, mirroring KokoroModel's load()/unload() lifecycle.
        """
        if self._loaded:
            logger.debug("Piper already loaded, skipping.")
            return
 
        self._voices = {}
        self._loaded = True
        logger.info("Piper backend ready (voices load lazily on first use).")

    def unload(self) -> None:
        """Release all cached Piper voices from memory."""
        if self._voices is not None:
            self._voices.clear()
            self._voices = None
            self._loaded = False
            logger.info("Piper voices unloaded.")

    def _get_voice(self, voice_id: str) -> "PiperVoice":
        from piper import PiperVoice
 
        if voice_id not in self._voices:
            model_path = self._voices_dir / f"{voice_id}.onnx"
            config_path = self._voices_dir / f"{voice_id}.onnx.json"
 
            if not model_path.exists() or not config_path.exists():
                print(os.getcwd())
                raise FileNotFoundError(
                    f"Piper voice '{voice_id}' not found in {self._voices_dir}. "
                    f"Expected {model_path.name} and {config_path.name}. "
                    "Download them from https://huggingface.co/rhasspy/piper-voices "
                    "and place both files in that directory."
                )
 
            logger.info(f"Loading Piper voice '{voice_id}'...")
            self._voices[voice_id] = PiperVoice.load(
                str(model_path),
                config_path=str(config_path),
                use_cuda=self._use_cuda,
            )
 
        return self._voices[voice_id]

    def synthesize(
        self,
        text: str,
        voice: str | None = None,
        speed: float = 1.0,
    ) -> Generator[bytes, None, None]:
        """
        Generate speech from text, yielding PCM byte chunks resampled to
        settings.audio.sample_rate (24kHz), matching Kokoro's output format
        exactly so the rest of the pipeline doesn't need to know which
        backend produced the audio.
        """
        if not self._loaded:
            raise RuntimeError("Piper model is not loaded. Call load() first.")
 
        if voice is None:
            voice = settings.model.piper_default_voice
 
        from piper import SynthesisConfig
 
        logger.debug(
            f"Synthesizing (Piper): voice={voice}, speed={speed}, text={text[:80]}..."
        )
 
        piper_voice = self._get_voice(voice)
        native_rate = piper_voice.config.sample_rate
        target_rate = settings.audio.sample_rate
 
        # Piper's length_scale is a DURATION multiplier (bigger = slower),
        # the inverse of our speed convention (bigger = faster).
        length_scale = 1.0 / speed if speed > 0 else 1.0
        syn_config = SynthesisConfig(length_scale=length_scale)
 
        chunk_index = 0
        for audio_chunk in piper_voice.synthesize(text, syn_config=syn_config):
            pcm_bytes = audio_chunk.audio_int16_bytes
 
            if not pcm_bytes:
                continue
 
            if native_rate != target_rate:
                pcm_bytes = _resample_pcm16(pcm_bytes, native_rate, target_rate)
                if not pcm_bytes:
                    continue
 
            chunk_index += 1
            logger.debug(f"  Chunk {chunk_index}: {len(pcm_bytes)} bytes")
            yield pcm_bytes
 
        logger.debug(f"Synthesis complete: {chunk_index} chunks yielded.")

    def list_voices(self) -> list[VoiceInfo]:
        """Return all known Piper voices."""
        return PIPER_VOICES.copy()
 
    @property
    def model_name(self) -> str:
        return "Piper"
 
    @property
    def is_loaded(self) -> bool:
        return self._loaded

def _resample_pcm16(pcm_bytes: bytes, orig_rate: int, target_rate: int) -> bytes:
    """
    Resample 16-bit signed mono PCM from orig_rate to target_rate.
 
    Uses polyphase resampling (scipy.signal.resample_poly) rather than a
    naive interpolation — cheap enough for real-time speech and avoids
    the aliasing artifacts a crude resample would introduce.
    """
    from scipy.signal import resample_poly
 
    audio_int16 = np.frombuffer(pcm_bytes, dtype=np.int16)
    if audio_int16.size == 0:
        return b""
 
    g = gcd(orig_rate, target_rate)
    up, down = target_rate // g, orig_rate // g
 
    audio_float = audio_int16.astype(np.float32) / 32768.0
    resampled = resample_poly(audio_float, up, down)
    resampled_clipped = np.clip(resampled, -1.0, 1.0)
    return (resampled_clipped * 32767).astype(np.int16).tobytes()
