"""
split_keyword.py - corta uma gravação longa falando "abacaxi" em amostras de 1 s para o Edge Impulse.

Acha cada palavra pela energia do som (onde sai do silêncio), centraliza a palavra numa
janela de 1 s e salva cada uma como WAV 16 kHz, mono, 16 bits (o formato do projeto).

Usar:
  python3 split_keyword.py --record 60          grava 60 s pelo microfone do Mac e já corta
  python3 split_keyword.py gravacao.m4a         corta um arquivo pronto (m4a do Gravador, wav, ...)
Opções:
  --label abacaxi    rótulo (vai no começo do nome do arquivo; o Edge Impulse lê o rótulo daí)
  --sens 3.0         sensibilidade: menor pega sons mais fracos, maior ignora mais ruído
  --out pasta        pasta de saída (padrão: <label>_1s)

Dica para gravar: fale a palavra a cada ~2 s, com pausa em silêncio entre uma e outra,
variando tom (normal, baixo, sussurro, rápido) e distância do notebook.
Depois: Edge Impulse > Data acquisition > Upload data > escolha os arquivos da pasta,
"Infer from filename" no rótulo e "Automatically split between training and testing".
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
import wave

import numpy as np

RATE = 16000
FRAME = 320          # 20 ms
WIN = RATE           # 1 s


def load_audio(path):
    """Lê o arquivo; se não for WAV 16 kHz mono 16 bits, converte com o afconvert do macOS."""
    def read_wav(p):
        with wave.open(p, "rb") as w:
            if w.getframerate() != RATE or w.getnchannels() != 1 or w.getsampwidth() != 2:
                return None
            return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.int16)

    if path.lower().endswith(".wav"):
        x = read_wav(path)
        if x is not None:
            return x
    tmp = os.path.join(tempfile.gettempdir(), "kw_convert.wav")
    try:
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", path, tmp],
                       check=True, capture_output=True)
    except (FileNotFoundError, subprocess.CalledProcessError) as e:
        sys.exit(f"Não consegui converter {path} ({e}). Grave com --record ou use um WAV 16 kHz mono.")
    return read_wav(tmp)


def record(seconds):
    import sounddevice as sd
    print(f"Gravando {seconds} s... fale \"abacaxi\" a cada ~2 s, com pausas.")
    for i in (3, 2, 1):
        print(f"  começa em {i}...")
        time.sleep(1)
    print("  GRAVANDO")
    x = sd.rec(int(seconds * RATE), samplerate=RATE, channels=1, dtype="int16")
    sd.wait()
    print("  fim")
    return x[:, 0].copy()


def find_words(x, sens):
    n = len(x) // FRAME
    frames = x[: n * FRAME].astype(np.float32).reshape(n, FRAME)
    rms = np.sqrt(np.mean(frames ** 2, axis=1)) + 1e-6
    floor = np.percentile(rms, 20)                   # nível do silêncio da própria gravação
    voiced = rms > floor * sens
    segs, start, gap = [], None, 0
    for i, v in enumerate(voiced):
        if v:
            if start is None:
                start = i
            gap = 0
        elif start is not None:
            gap += 1
            if gap > 8:                              # 160 ms de silêncio fecha a palavra
                segs.append((start, i - gap + 1))
                start, gap = None, 0
    if start is not None:
        segs.append((start, n))
    words, skipped = [], []
    for a, b in segs:
        dur = (b - a) * FRAME / RATE
        if dur < 0.20:
            continue                                 # estalo/ruído curto
        if dur > 1.0:
            skipped.append((a * FRAME / RATE, dur))  # longo demais para caber em 1 s
            continue
        words.append(((a + b) // 2) * FRAME)          # centro da palavra (amostra)
    return words, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?")
    ap.add_argument("--record", type=float, default=0)
    ap.add_argument("--label", default="abacaxi")
    ap.add_argument("--sens", type=float, default=3.0)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    if args.record:
        x = record(args.record)
    elif args.file:
        x = load_audio(args.file)
    else:
        sys.exit("Use --record 60 ou passe um arquivo de áudio.")

    words, skipped = find_words(x, args.sens)
    out = args.out or f"{args.label}_1s"
    os.makedirs(out, exist_ok=True)
    stamp = time.strftime("%H%M%S")
    for k, c in enumerate(words, 1):
        a = min(max(c - WIN // 2, 0), max(len(x) - WIN, 0))
        seg = x[a:a + WIN]
        if len(seg) < WIN:
            seg = np.pad(seg, (0, WIN - len(seg)))
        name = os.path.join(out, f"{args.label}.{stamp}_{k:03d}.wav")
        with wave.open(name, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(seg.astype("<i2").tobytes())
        print(f"  {k:3d}: palavra em {c / RATE:6.2f} s -> {name}")
    for t, d in skipped:
        print(f"  ignorado: som de {d:.1f} s em {t:.1f} s (longo demais; fale com pausas)")
    print(f"\n{len(words)} amostras salvas em {out}/")
    if len(words) == 0:
        print("Nada encontrado: tente --sens 2 (mais sensível).")


if __name__ == "__main__":
    main()
