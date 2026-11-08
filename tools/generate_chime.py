"""Package the approved audition's 800 ms tone as embedded device PCM."""
from pathlib import Path
import wave
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'design/chime-options/03-sparkle.wav'
TARGET=ROOT/'vendor/esp-mosaico-vibe-legacy-v26.0.1/projects/codex_pet/components/app_notify/chime.pcm'
with wave.open(str(SOURCE),'rb') as audio:
    assert (audio.getnchannels(),audio.getsampwidth(),audio.getframerate())==(1,2,16000)
    audio.setpos(768)  # Remove the audition padding; the device player supplies startup silence.
    pcm=audio.readframes(12800)
assert len(pcm)==25600
TARGET.write_bytes(pcm)
print('Approved three-note chime packaged:',len(pcm),'bytes')
