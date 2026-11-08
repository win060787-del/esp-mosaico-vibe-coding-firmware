"""Rebuild the Noto Sans SC subset from the official Google Fonts variable TTF.

Usage: python tools/build_font.py /path/to/NotoSansSC[wght].ttf
Source: https://github.com/google/fonts/tree/main/ofl/notosanssc (SIL OFL 1.1).
"""
import sys
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools import subset

font = TTFont(sys.argv[1])
instantiateVariableFont(font, {'wght': 400}, inplace=True)
chars = set(range(32, 127)) | set(range(0x3000, 0x3040)) | {0x2039, 0x2026, 0xfffd, 0xb7, 0x2190}
for hi in range(0xa1, 0xf8):
    for lo in range(0xa1, 0xff):
        try:
            chars.update(map(ord, bytes([hi, lo]).decode('gb2312')))
        except UnicodeDecodeError:
            pass
options = subset.Options()
options.name_IDs = ['*']
options.name_legacy = True
options.name_languages = ['*']
builder = subset.Subsetter(options=options)
builder.populate(unicodes=chars)
builder.subset(font)
font.save(Path(__file__).resolve().parents[1] / 'components/app_pet/ui/fonts/NotoSansSC.ttf')
