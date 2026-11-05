"""Play a movie with sound: ysp.video on a ysp.screen, its soundtrack on a ysp.audio device.

    python example_video.py                   # a movie made here, in a window, the default output
    python example_video.py clip.mp4 clip.wav --full
    python example_video.py --sim --null      # no window, no sound card (CI)

With no clip it writes a 3 s frame sequence (a gray gradient and a moving
bar, low contrast) and a WAV of the same length: clicks on each second.
An MPEG-1 or MP4 clip needs its index beside it (ysp.video.make_index); a
WAV must be at the device's rate and channel count and last as long as the
movie, to the sample. Shift+Esc stops it. It prints the frame decisions and
the audio-video error that the records report (the flip onsets against the
sound's clock; no photodiode or microphone, so not light against sound).
"""
import argparse
import array
import collections
import os
import tempfile
import wave

import ysp.audio as yau
import ysp.gfx as ygfx
import ysp.screen as yscr
import ysp.video as yvid


def make_clip(d, rate, channels):
    """3 s at 30 fps, and its WAV: 3 x rate samples (the movie's duration)."""
    w, h, fps, n = 320, 180, 30, 90
    clip = os.path.join(d, "example.yspseq")
    with yvid.SeqWriter(clip, w, h, fps) as wr:
        for i in range(n):
            bar = (i * w) // n
            row = bytearray()
            for x in range(w):
                v = 150 if bar <= x < bar + 8 else 112 + (x * 32) // w
                row += bytes((v, v, v, 255))
            wr.write(bytes(row) * h)
    wav = os.path.join(d, "example.wav")
    x = array.array("h", [0]) * (3 * rate * channels)
    for second in range(3):
        for k in range(int(rate * 0.005)):          # a 5 ms click at each second
            for c in range(channels):
                x[(second * rate + k) * channels + c] = 3000 if (k // 24) % 2 else -3000
    with wave.open(wav, "wb") as f:
        f.setnchannels(channels)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(x.tobytes())
    return clip, wav


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("clip", nargs="?")
    ap.add_argument("wav", nargs="?")
    ap.add_argument("--full", action="store_true", help="fullscreen")
    ap.add_argument("--sim", action="store_true", help="the simulated display")
    ap.add_argument("--null", action="store_true", help="miniaudio's null device")
    a = ap.parse_args()

    with yau.Audio(backend="null" if a.null else None) as au, tempfile.TemporaryDirectory() as d:
        caps = au.caps()
        clip, wav = (a.clip, a.wav) if a.clip else make_clip(d, caps["rate"], caps["channels"])
        opts = {"backend": "sim"} if a.sim else {"windowed": not a.full, "size": (960, 540)}
        with yscr.Screen(**opts) as s, ygfx.Gfx(s, background=0.45) as g, yvid.Movie(g, clip) as m:
            print(m.describe())
            if wav:
                m.soundtrack(au, wav)                # the movie clock follows the sound
            m.play()
            while True:
                try:
                    s.begin()
                except yscr.Abort:
                    break
                playing = m.update()                 # the frame due at this frame's predicted onset
                g.begin()
                m.draw()
                g.end()
                s.flip()
                if not playing:
                    break
            s.wait_flip()
            frames = m.records()
            events = m.events()
            info = m.info

    counts = collections.Counter((yvid.DECISION_NAMES[r.decision], yvid.WHY_NAMES[r.why]) for r in frames)
    print(f"display frames {len(frames):d}; video frames shown {info['shown']:d}, repeated {info['repeated']:d}, "
          f"dropped {info['dropped']:d}")
    for (decision, why), n in sorted(counts.items()):
        print(f"  {decision!s:<8} {why!s:<12} {n:d}")
    av = sorted(r.residual * 1e3 for r in frames if r.flags & yvid.F_AUDIO_CLOCK and r.decision == yvid.SHOWN)
    if av:
        p1, p50, p99 = av[len(av) // 100], av[len(av) // 2], av[(len(av) * 99) // 100]
        print(f"flip onset minus the frame's time on the audio clock, shown frames: p1 {p1:.3f}, p50 {p50:.3f}, p99 {p99:.3f} ms")
    for e in events:
        if e["kind"] in ("play", "sound", "end"):
            print(f"  {e['kind']} at {e['t']:.6f} s")


if __name__ == "__main__":
    main()
