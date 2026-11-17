"""
pc_bridge.py - o PC faz o papel dos componentes que ainda não estão na placa.

Tudo passa pelo cabo USB, como passaria pelos fios dos módulos reais. O código de decisão
continua 100% na ESP32; o PC só imita o hardware:

  * Microfone: quando a placa avisa "LISTEN", manda o áudio CRU do microfone do Mac
    (16 kHz, mono, 16 bits) em pacotes 0xAA 0x55 + 500 amostras. A palavra secreta é
    reconhecida NA PLACA (modelo do Edge Impulse).
  * GPS NEO-6M: quando a placa avisa "GPS_ON", manda frases NMEA $GPRMC a 1 Hz, como o
    módulo real. Nos primeiros --cold segundos sem posição (partida a frio; 27 s no
    datasheet), depois com a localização real do Mac (ou a de teste, com --fixed). Para em "GPS_OFF".
  * SIM800L: responde aos comandos AT de SMS como o módulo (OK, ">", "+CMGS"), depois de
    --sms segundos, e grava o texto do SMS no log.

Também mostra e grava em arquivo tudo o que a placa imprime, com horário.

Instalar:  pip3 install pyserial sounddevice numpy pyobjc-framework-CoreLocation
Usar:      python3 pc_bridge.py                (procura a porta da placa sozinho)
           python3 pc_bridge.py --no-mic       (sem microfone, para o modo botão)
           python3 pc_bridge.py --levels       (só mostra o nível do microfone, para calibrar --vad)
           python3 pc_bridge.py --show-coords  (mostra as coordenadas no log; não compartilhe esse log)
Feche este script antes de gravar o firmware pela Arduino IDE (a porta não abre nos dois).
"""
import argparse
import datetime as dt
import os
import queue
import re
import sys
import threading
import time

import serial
import serial.tools.list_ports

RATE = 16000           # Hz, igual ao projeto do Edge Impulse
FRAME = 500            # amostras por pacote, igual a FRAME_SAMPLES no firmware
SYNC = b"\xAA\x55"
TOKENS = re.compile(r"(BOTAO|GPS_ON|GPS_OFF|GPS_FIX|GPS_NOFIX|LISTEN|START|KWS|RESULT|NOACK|SMS_|PAIR|PRONTO|END,|MEAS|AT\+|AT$)")
TEST_LAT, TEST_LON = -22.9068, -43.1729   # posição de teste fixa (centro do Rio), usada se o Mac não der a localização
POS = {"lat": TEST_LAT, "lon": TEST_LON}  # posição que o GPS imitado vai mandar


def mac_location(timeout=20):
    """Pega a localização real do Mac (Serviços de Localização do macOS, por Wi-Fi).
    1º tenta pelo pyobjc (pip3 install pyobjc-framework-CoreLocation);
    2º pelo CoreLocationCLI (brew install corelocationcli). Devolve (lat, lon, precisão_m) ou None."""
    try:
        import CoreLocation
        from Foundation import NSDate, NSRunLoop
        mgr = CoreLocation.CLLocationManager.alloc().init()
        mgr.setDesiredAccuracy_(CoreLocation.kCLLocationAccuracyBest)
        if hasattr(mgr, "requestWhenInUseAuthorization"):
            mgr.requestWhenInUseAuthorization()
        mgr.startUpdatingLocation()
        t = time.time()
        while time.time() - t < timeout:
            loc = mgr.location()
            if loc is not None:
                c = loc.coordinate()
                mgr.stopUpdatingLocation()
                return c.latitude, c.longitude, loc.horizontalAccuracy()
            NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.5))
        mgr.stopUpdatingLocation()
    except Exception:
        pass
    try:
        import subprocess
        r = subprocess.run(["CoreLocationCLI", "-once", "-format", "%latitude %longitude %h_accuracy"],
                           capture_output=True, text=True, timeout=timeout)
        lat, lon, acc = r.stdout.split()[:3]
        return float(lat), float(lon), float(acc)
    except Exception:
        return None


def bot_token():
    return os.environ.get("TELEGRAM_BOT_TOKEN")


def telegram_send(token, chat_id, text):
    import json
    import urllib.parse
    import urllib.request
    data = urllib.parse.urlencode({"chat_id": chat_id, "text": text}).encode()
    with urllib.request.urlopen(f"https://api.telegram.org/bot{token}/sendMessage", data, timeout=10) as r:
        return json.load(r).get("ok", False)


def find_port():
    for p in serial.tools.list_ports.comports():
        if "usb" in (p.device + " " + (p.description or "")).lower():
            return p.device
    return None


def nmea_checksum(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return f"{c:02X}"


def deg_to_nmea(value, is_lat):
    hemi = ("N" if value >= 0 else "S") if is_lat else ("E" if value >= 0 else "W")
    v = abs(value)
    d = int(v)
    m = (v - d) * 60
    return (f"{d:02d}{m:07.4f}" if is_lat else f"{d:03d}{m:07.4f}"), hemi


def gprmc(fix):
    now = dt.datetime.utcnow()
    hhmmss, ddmmyy = now.strftime("%H%M%S.00"), now.strftime("%d%m%y")
    if fix:
        lat, ns = deg_to_nmea(POS["lat"], True)
        lon, ew = deg_to_nmea(POS["lon"], False)
        body = f"GPRMC,{hhmmss},A,{lat},{ns},{lon},{ew},0.0,0.0,{ddmmyy},,,A"
    else:
        body = f"GPRMC,{hhmmss},V,,,,,,,{ddmmyy},,,N"
    return f"${body}*{nmea_checksum(body)}\r\n"


def mask(text):
    return re.sub(r"-?\d+\.\d{3,},\s*-?\d+\.\d{3,}", "*,*", text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--no-mic", action="store_true", help="não abre o microfone (modo botão)")
    ap.add_argument("--cold", type=float, default=27.0, help="segundos até o GPS imitado ter posição")
    ap.add_argument("--sms", type=float, default=3.0, help="segundos até o SIM800L imitado confirmar o SMS")
    ap.add_argument("--vad", type=float, default=800.0, help="nível RMS que conta como fala")
    ap.add_argument("--levels", action="store_true", help="só mostra o nível do microfone por 10 s")
    ap.add_argument("--show-coords", action="store_true", help="mostra as coordenadas no log")
    ap.add_argument("--fixed", action="store_true", help="usa a posição de teste fixa em vez da localização do Mac")
    ap.add_argument("--telegram", metavar="CHAT_ID", default=None,
                    help="entrega cada SMS do SIM800L imitado no Telegram desse chat_id (faz o papel da rede GSM)")
    args = ap.parse_args()

    if not args.no_mic or args.levels:
        import numpy as np
        import sounddevice as sd

    if args.levels:
        print("Fale e fique em silêncio para ver os níveis (RMS)...")
        with sd.InputStream(samplerate=RATE, channels=1, dtype="int16", blocksize=FRAME) as st:
            t_end = time.time() + 10
            while time.time() < t_end:
                data, _ = st.read(FRAME)
                print(f"{np.sqrt(np.mean(data.astype(np.float32) ** 2)):8.0f}")
        return

    if not args.fixed:
        print("Pegando a localização do Mac...")
        loc = mac_location()
        if loc:
            POS["lat"], POS["lon"] = loc[0], loc[1]
            shown = f"{loc[0]:.6f},{loc[1]:.6f}" if args.show_coords else "*,*"
            print(f"GPS imitado vai usar a localização real do Mac: {shown} (precisão ~{loc[2]:.0f} m)")
        else:
            print("Não consegui a localização do Mac (veja a permissão em Ajustes > Privacidade > Serviços de "
                  "Localização). Usando a posição de teste fixa.")

    port = args.port or find_port()
    if not port:
        sys.exit("Placa não encontrada. Rode com --port /dev/cu.usbserial-XXXX")
    ser = serial.Serial(port, args.baud, timeout=0.05)
    wlock = threading.Lock()

    def send(data):
        with wlock:
            ser.write(data)

    logname = dt.datetime.now().strftime("log_%Y%m%d_%H%M%S.txt")
    log = open(logname, "w", buffering=1)
    print(f"Porta {port} @ {args.baud}. Log em {logname}. Ctrl+C para sair.")

    def out(text):
        if not args.show_coords:
            text = mask(text)
        stamp = dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        line = f"{stamp} {text}"
        print(line)
        log.write(line + "\n")

    token = bot_token() if args.telegram else None
    if args.telegram and not token:
        print("Configure TELEGRAM_BOT_TOKEN; os SMS ficam só no log.")
    state = {"listen": False, "gps_on": False, "gps_t0": 0.0, "last_voice": 0.0}
    marks, kws_times, latencies = [], [], []     # Enter apertado (fala), detecções, latências
    stop = threading.Event()

    # --- GPS imitado: 1 frase por segundo enquanto ligado ---
    def gps_loop():
        while not stop.is_set():
            if state["gps_on"]:
                fix = time.time() - state["gps_t0"] >= args.cold
                send(gprmc(fix).encode())
            time.sleep(1.0)

    # --- SIM800L imitado + leitura do log da placa ---
    def reader():
        buf = b""
        sms_body = None
        while not stop.is_set():
            buf += ser.read(512)
            while True:
                if sms_body is not None:          # depois do ">", o texto vai até o Ctrl+Z (0x1A)
                    if b"\x1a" not in buf:
                        break
                    body, buf = buf.split(b"\x1a", 1)
                    text = body.decode(errors="replace").strip()
                    out(f"[SIM800L] SMS: {text}")
                    sms_body = None
                    if args.telegram and token:
                        def deliver(msg=text):
                            try:
                                ok = telegram_send(token, args.telegram, "SMS do botão: " + msg)
                                out(f"[SIM800L] entregue no Telegram: {'ok' if ok else 'falhou'}")
                            except Exception as e:
                                out(f"[SIM800L] Telegram falhou: {e}")
                        threading.Thread(target=deliver, daemon=True).start()

                    def confirm():
                        time.sleep(args.sms)
                        send(b"\r\n+CMGS: 1\r\n\r\nOK\r\n")
                    threading.Thread(target=confirm, daemon=True).start()
                    continue
                if b"\n" not in buf:
                    break
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode(errors="replace").strip()
                # ao acordar a ESP32 solta lixo sem quebra de linha; recorta a partir do comando conhecido
                m = TOKENS.search(text)
                if m and m.start() > 0:
                    text = text[m.start():]
                if not text:
                    continue
                if not text.startswith("LISTEN") and TOKENS.match(text):
                    state["listen"] = False          # qualquer atividade de emergência: para o áudio
                if text.startswith("AT"):
                    out(f"[SIM800L] <- {text}")
                    if text.startswith("AT+CMGS"):
                        send(b"\r\n> ")
                        sms_body = b""
                    else:
                        send(b"\r\nOK\r\n")
                    continue
                if text.startswith("LISTEN"):
                    state["listen"] = True
                    if text == "LISTEN":             # lembrete periódico da placa: não polui o log
                        continue
                elif text.startswith("START") or text.startswith("KWS"):
                    state["listen"] = False      # placa saiu do modo ouvir: para de mandar áudio
                elif text == "GPS_ON":
                    state["gps_on"], state["gps_t0"] = True, time.time()
                    out("[GPS] ligado (partida a frio)")
                    continue
                elif text == "GPS_OFF":
                    state["gps_on"] = False
                    out("[GPS] desligado")
                    continue
                extra = ""
                if text.startswith("KWS"):
                    now = time.time()
                    kws_times.append(now)
                    if state["last_voice"] and now - state["last_voice"] < 3.0:
                        lat = (now - state["last_voice"]) * 1000
                        latencies.append(lat)
                        extra = f"  [fim da fala -> detecção: {lat:.0f} ms]"
                out(text + extra)

    def keyboard():
        # aperte Enter logo depois de falar a palavra: o script conta acertos e erros sozinho
        while not stop.is_set():
            line = sys.stdin.readline()
            if not line:
                return
            marks.append(time.time())
            out(f"FALA,{len(marks)}")

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=gps_loop, daemon=True).start()
    threading.Thread(target=keyboard, daemon=True).start()

    try:
        if args.no_mic:
            while True:
                time.sleep(1)
        q = queue.Queue()

        def on_audio(indata, frames, t, status):
            q.put(indata[:, 0].copy())

        from collections import deque
        recent = deque(maxlen=160)               # ~5 s de níveis do microfone
        with sd.InputStream(samplerate=RATE, channels=1, dtype="int16", blocksize=FRAME, callback=on_audio):
            while True:
                chunk = q.get()
                rms = float(np.sqrt(np.mean(chunk.astype(np.float32) ** 2)))
                recent.append(rms)
                floor = float(np.percentile(recent, 20)) if len(recent) > 20 else rms
                if rms > max(floor * 4, args.vad / 4):   # fala = bem acima do ruído de fundo do momento
                    state["last_voice"] = time.time()
                if state["listen"]:              # só manda áudio enquanto a placa está ouvindo
                    send(SYNC + chunk.astype("<i2").tobytes())
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        time.sleep(0.2)
        ser.close()
        if marks:
            used, hits = set(), 0
            for m in marks:                      # acerto = detecção até 3 s antes ou depois do Enter
                cand = [i for i, k in enumerate(kws_times) if abs(k - m) <= 3.0 and i not in used]
                if cand:
                    used.add(cand[0]); hits += 1
            extra_kws = len(kws_times) - len(used)
            summary = (f"RESUMO,falas={len(marks)},detectadas={hits},taxa={100 * hits / len(marks):.0f}%,"
                       f"deteccoes_sem_fala={extra_kws}")
            if latencies:
                ls = sorted(latencies)
                summary += f",latencia_mediana_ms={ls[len(ls) // 2]:.0f}"
            out(summary)
        log.close()
        print(f"\nLog salvo em {logname}")


if __name__ == "__main__":
    main()
