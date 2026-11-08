"""Explicit physical self-test and telemetry through the official device CLI."""
import argparse
from device_network import rpc
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--play',action='store_true');parser.add_argument('--audio-only',action='store_true');args=parser.parse_args()
    print(rpc(4,{'audio_only':True}) if args.play and args.audio_only else rpc(4 if args.play else 5))
