"""Banco de ensaio do acionamento. A ESP32 executa o classificador e lê o botão."""
import argparse
import csv
from datetime import datetime
from pathlib import Path
import queue
import random
import time

import serial
from serial.tools import list_ports
import sounddevice as sd

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--porta")
parser.add_argument("--tentativas", type=int, default=20, help="quantidade por método")
parser.add_argument("--negativo", action="store_true", help="30 minutos sem palavra-alvo")
args = parser.parse_args()
if args.tentativas < 1:
    parser.error("Use pelo menos uma tentativa por método.")
ports = [p.device for p in list_ports.comports() if p.vid is not None]
if not args.porta and len(ports) != 1:
    parser.error(f"Escolha --porta. Portas USB encontradas: {ports}")
port = args.porta or ports[0]
audio = queue.Queue(maxsize=32)
errors = queue.SimpleQueue()

def microphone(data, frames, timing, status):
    if status:
        errors.put(str(status))
    try:
        audio.put_nowait(bytes(data))
    except queue.Full:
        errors.put("fila de áudio cheia")

out = Path("ensaio_" + datetime.now().strftime("%Y%m%d_%H%M%S_%f") + ".csv")
with serial.Serial(port, 460800, timeout=0, write_timeout=1) as board, out.open("x") as file:
    writer = csv.writer(file)
    writer.writerow(["tempo_monotonico_s", "evento", "tentativa", "esperado", "observado", "latencia_ms"])
    trial = 0
    expected = ""
    def record(event, observed="", latency=""):
        writer.writerow([f"{time.monotonic():.4f}", event, trial, expected, observed, latency])
        file.flush()

    time.sleep(2)
    board.reset_input_buffer()
    pending = bytearray()
    last_count = -1
    progress = time.monotonic()
    ready = False
    def pump():
        global last_count, progress, ready
        if not errors.empty():
            raise RuntimeError(errors.get())
        try:
            chunk = audio.get(timeout=0.15)
        except queue.Empty:
            chunk = None
        if chunk:
            board.write(b"\xaa\x55" + chunk)
        pending.extend(board.read(board.in_waiting))
        events = []
        while b"\n" in pending:
            line, _, rest = pending.partition(b"\n")
            pending[:] = rest
            line = line.decode(errors="replace").strip()
            if line.startswith("ALIVE,"):
                count = int(line.split(",")[1])
                if count < last_count:
                    raise RuntimeError("A placa reiniciou.")
                if count > last_count:
                    progress = time.monotonic()
                    ready = count >= 4
                last_count = count
            elif line.startswith("START,"):
                events.append(line.split(",")[1])
            elif line.startswith("ERROR"):
                raise RuntimeError(line)
        if time.monotonic() - progress > 5:
            raise RuntimeError("Sem progresso da inferência: confira sketch, USB e microfone.")
        return events

    def interval(seconds, label):
        end = time.monotonic() + seconds
        count = 0
        while time.monotonic() < end:
            for source in pump():
                record(label, source)
                if source == "voz":
                    count += 1
        return count

    try:
        with sd.RawInputStream(samplerate=16000, blocksize=500, channels=1, dtype="int16", callback=microphone):
            print("Aguardando inferência na placa...", flush=True)
            while not ready:
                pump()
            if args.negativo:
                print("Começou. Converse normalmente por 30 minutos, sem a palavra-alvo.")
                record("inicio_negativo")
                total = interval(1800, "disparo_no_negativo")
                record("fim_negativo", str(total))
                print(f"30 minutos concluídos. Disparos por voz: {total}.")
            else:
                order = ["voz", "botao"] * args.tentativas
                random.Random(42).shuffle(order)
                correct = {"voz": 0, "botao": 0}
                print("Aguarde o sinal em cada rodada. Faça somente a ação pedida.")
                for trial, expected in enumerate(order, 1):
                    print(f"\n{trial}/{len(order)} · Prepare-se...", flush=True)
                    interval(2, "fora_da_janela")
                    command = "FALE ABACAXI" if expected == "voz" else "APERTE E SOLTE O BOTÃO"
                    print(command, flush=True)
                    start = time.monotonic()
                    record("sinal")
                    first = ""
                    while time.monotonic() - start < 4:
                        for source in pump():
                            ms = round(1000 * (time.monotonic() - start))
                            record("deteccao" if not first else "extra", source, ms)
                            if not first:
                                first = source
                    record("resultado", first or "sem_disparo")
                    correct[expected] += int(first == expected)
                    print("Resultado:", first or "sem disparo")
                record("fim_comparacao")
                print("Primeiro disparo correto:", correct, f"de {args.tentativas} por método.")
    except (Exception, KeyboardInterrupt) as exc:
        record("interrompido", str(exc))
        raise
    finally:
        print("CSV:", out.resolve())
