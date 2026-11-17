import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import messagebox, ttk

import cv2


ROOT = Path(__file__).resolve().parent
BRIDGE = ROOT / "tools" / "pc_bridge.py"
OUTPUT = Path.home() / "Movies" / "DemoBotao"
OUTPUT.mkdir(parents=True, exist_ok=True)

if not BRIDGE.is_file():
    sys.exit("Coloque demobotao.py na pasta demo, ao lado da pasta tools.")

window = tk.Tk()
window.title("Demonstração • Botão de emergência")
window.geometry("620x730")

panel = ttk.Frame(window, padding=12)
panel.pack(fill="both", expand=True)

camera_view = ttk.Label(panel, text="Abrindo câmera…", anchor="center")
camera_view.pack(fill="x")

status = tk.StringVar(value="Iniciando a ponte com a ESP32…")
ttk.Label(panel, textvariable=status, wraplength=575,
          font=("Helvetica", 16, "bold")).pack(anchor="w", pady=(10, 5))

timeline = tk.Listbox(panel, height=10, font=("Helvetica", 13))
timeline.pack(fill="both", expand=True)

controls = ttk.Frame(panel)
controls.pack(fill="x", pady=(8, 0))
record_status = tk.StringVar(value="Gravação parada")
ttk.Label(controls, textvariable=record_status).pack(side="right")

messages = queue.Queue()
camera = cv2.VideoCapture(int(os.getenv("DEMO_CAMERA", "0")), cv2.CAP_AVFOUNDATION)
recorder = None
movie = None
stopping = False
closing = False
last_listen = False
first_frame = False

def show(text):
    status.set(text)
    timeline.insert("end", f"{datetime.now():%H:%M:%S}   {text}")
    timeline.see("end")

def translate(line):
    global last_listen

    # A ponte também imprime LISTEN periodicamente. Mostra uma vez por ciclo.
    if re.search(r"\bLISTEN\b", line):
        if not last_listen:
            show("Pronta para o botão ou a palavra “abacaxi”")
        last_listen = True
        return
    if "KWS,abacaxi," in line:
        last_listen = False
        show("Palavra “abacaxi” reconhecida pela ESP32")
    elif match := re.search(r"START,\d+,(botao|voz),", line):
        last_listen = False
        source = "pelo botão" if match[1] == "botao" else "pela voz"
        show(f"Emergência iniciada {source}")
    elif match := re.search(r"RESULT,\d+,(\d+),[\d.]+,celular,(\d+)", line):
        show(f"Envio {match[1]} • posição recebida por Bluetooth em {match[2]} ms")
    elif re.search(r"\bNOACK,", line):
        show("Sem posição válida pelo Bluetooth • iniciando o GPS de reserva")
    elif "[GPS] ligado" in line or "GPS_ON" in line:
        show("GPS emulado ligado • aguardando posição")
    elif "GPS_NOFIX," in line:
        show("GPS sem posição válida nesta tentativa")
    elif "GPS_FIX," in line:
        show("Posição do GPS emulado aceita pela ESP32")
    elif match := re.search(r"SMS_(OK|FAIL),\d+,(\d+),", line):
        result = "aceito pelo modem emulado" if match[1] == "OK" else "falhou"
        show(f"Envio {match[2]} • SMS {result}")
    elif "entregue no Telegram: ok" in line:
        show("Entrega confirmada no Telegram")
    elif "Telegram falhou" in line or "entregue no Telegram: falhou" in line:
        show("Falha na entrega ao Telegram")
    elif re.search(r"\bEND,\d+,", line):
        last_listen = False
        show("Emergência encerrada pela usuária")
    elif "Placa não encontrada" in line:
        show("ESP32 não encontrada • confira o cabo USB e a porta")

# A ponte usa o microfone do Mac. O painel continua usando Bluetooth.
# Os argumentos adicionais, por exemplo --telegram, seguem para a ponte.
bridge = subprocess.Popen(
    [sys.executable, "-u", str(BRIDGE), "--fixed", *sys.argv[1:]],
    cwd=OUTPUT,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    errors="replace",
)

def read_bridge():
    for line in bridge.stdout:
        print(line, end="", flush=True)
        messages.put(line.strip())
    messages.put(None)

threading.Thread(target=read_bridge, daemon=True).start()

def refresh():
    global first_frame
    while not messages.empty():
        line = messages.get_nowait()
        if line is None:
            show("Ponte encerrada • veja o diagnóstico no Terminal")
        else:
            translate(line)

    if camera.isOpened():
        ok, frame = camera.read()
        if ok:
            h, w = frame.shape[:2]
            width = min(560, w)
            frame = cv2.resize(frame, (width, round(h * width / w)))
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            h, w = rgb.shape[:2]
            ppm = f"P6\n{w} {h}\n255\n".encode() + rgb.tobytes()
            picture = tk.PhotoImage(data=ppm, format="PPM")
            camera_view.configure(image=picture, text="")
            camera_view.image = picture
            first_frame = True
    elif not first_frame:
        camera_view.configure(text="Câmera indisponível • confira a permissão do Terminal")
    window.after(80, refresh)

def watch_recording():
    global recorder, stopping
    if recorder is None:
        return
    if recorder.poll() is None:
        window.after(250, watch_recording)
        return
    saved = movie.is_file() and movie.stat().st_size > 0
    recorder = None
    stopping = False
    record_button.configure(text="Gravar tela", state="normal")
    record_status.set("Vídeo salvo" if saved else "Gravação não concluída")
    if saved:
        show(f"Vídeo salvo em Filmes/DemoBotao/{movie.name}")
    else:
        show("Confira a permissão de gravação de tela e a mensagem no Terminal")
    if closing:
        window.destroy()

def stop_recording():
    global stopping
    if recorder and recorder.poll() is None and not stopping:
        stopping = True
        record_button.configure(state="disabled")
        record_status.set("Finalizando o vídeo…")
        recorder.send_signal(signal.SIGINT)

def toggle_recording():
    global recorder, movie
    if recorder is not None:
        stop_recording()
        return
    if not first_frame:
        messagebox.showinfo("Câmera", "Espere a imagem da placa aparecer antes de gravar.")
        return
    movie = OUTPUT / f"demonstracao_{datetime.now():%Y%m%d_%H%M%S}.mov"
    recorder = subprocess.Popen(
        ["/usr/sbin/screencapture", "-v", "-D", "1", str(movie)]
    )
    record_button.configure(text="Parar e salvar")
    record_status.set("Gravando a tela principal")
    window.after(250, watch_recording)

record_button = ttk.Button(controls, text="Gravar tela", command=toggle_recording)
record_button.pack(side="left")

def close():
    global closing
    closing = True
    camera.release()
    if bridge.poll() is None:
        bridge.terminate()
    if recorder is not None:
        stop_recording()
    else:
        window.destroy()

window.protocol("WM_DELETE_WINDOW", close)
window.after(100, refresh)
window.mainloop()