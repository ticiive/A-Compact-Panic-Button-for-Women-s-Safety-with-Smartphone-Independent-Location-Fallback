"""
Simulador de autonomia do botão de pânico durante uma emergência.

Método (Caracas et al., SECON 2011, p. 62 dos anais / p. 8 do PDF):
    L = C / (q_sleep + q_active), calculado por ciclo de duração Δ.
Para cada caminho, a carga de um envio é a soma de corrente × tempo de cada etapa;
no resto do ciclo o sistema fica dormindo.

Edite só o bloco PARÂMETROS. Cada valor traz a fonte e a página.
Saídas: energy_lifetime.pdf (figura para o artigo) e a tabela no terminal.
"""
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# =============================== PARÂMETROS ===============================
C_MAH = 2000.0          # bateria LiPo 2000 mAh (mesma capacidade de Caracas e Purnima)

# ESP32 (SoC) - Espressif, "ESP32 Series Datasheet" v5.3, Table 4-2, p. 30
I_ESP_DEEP   = 0.010    # mA, Deep-sleep "RTC timer + RTC memory": 10 µA
I_ESP_AWAKE  = 68.0     # mA, Modem-sleep 240 MHz dual-core "30 mA ~ 68 mA" (pior caso; CPU ligada, rádio desligado)
# ESP32 com Bluetooth - Espressif, "ESP-WROOM-32 Datasheet" (2016), Table 5, p. 7
I_ESP_BLE    = 120.0    # mA, "Wi-Fi / BT Tx packet, 0 dBm: 120 mA" (pior caso; Rx/listening é 80~90 mA)

# GPS NEO-6M - u-blox, "NEO-6 Data Sheet" GPS.G6-HW-09005-E
I_GPS_ACQ    = 47.0     # mA, Table 11 p. 15, Acquisition (média de 30 s após partida a frio)
T_GPS_COLD   = 27.0     # s,  Table 2 p. 6, TTFF Cold Start NEO-6M (sem aiding, -130 dBm)

# GSM SIM800L - SIMCom, "SIM800H&SIM800L Hardware Design" V2.02, Table 49, p. 60
I_GSM_SLEEP  = 1.04     # mA, Sleep mode (AT+CFUN=1, BS-PA-MFRMS=9), registrado na rede
I_GSM_IDLE   = 17.38    # mA, Idle mode (AT+CFUN=1), usado durante o registro na rede
I_GSM_OFF    = 0.150    # mA, Power down mode (150 µA typ), fora de emergência
I_GSM_TX     = 238.44   # mA, Voice call PCL=5 EGSM900 (maior valor; o manual não dá corrente de SMS)
T_SMS        = 3.0      # s,  tempo de envio do SMS, Purnima et al. 2025, Table I, p. 1886

# Microfone I2S INMP441 (TDK InvenSense, DS-INMP441-00 rev 1.1), Table 1, p. 4
I_MIC        = 2.5      # mA, Supply Current, Normal Mode, VDD = 3.3 V: 2.2 typ / 2.5 max (usado o máximo)
# ESP32 ouvindo (modo voz): CPU ligada lendo o microfone e rodando o modelo, rádio desligado (Table 4-2, p. 30)
I_ESP_LISTEN_240 = 68.0 # mA, Modem-sleep 240 MHz dual-core, limite superior de "30 mA ~ 68 mA"
I_ESP_LISTEN_160 = 44.0 # mA, Modem-sleep 160 MHz dual-core, limite superior de "27 mA ~ 44 mA"
# 80 MHz não cabe: o MFE leva 350 ms por janela de 1 s a 240 MHz (Edge Impulse, ESP-EYE); por fatia de 250 ms
# são ~90 ms + 11 ms do classificador; a 80 MHz (~3x mais lento) passaria de 250 ms.

# Caminho do celular - medido no protótipo (Tabela I do artigo, mediana em deep sleep)
T_BLE        = 2.602    # s,  proxy histórico: inicialização até o ACK, incluindo requisição Telegram na AP1

DELTAS_S = np.arange(40, 601, 1)        # intervalos simulados (s)
MARK_S   = [60, 180, 240, 300, 600]     # intervalos destacados na tabela
BAND_S   = (180, 300)                   # faixa candidata (3 a 5 min), usada na conta do SIM800L
POINTS_S = np.arange(60, 601, 60)      # pontos marcados e rotulados no gráfico (1 a 10 min)
CHOSEN_S = 240                          # intervalo escolhido, destacado no gráfico (4 min)
# =========================================================================

# Durante a emergência, entre envios: ESP32 em deep sleep, GPS sem alimentação, SIM800L em sleep (registrado).
I_SLEEP = I_ESP_DEEP + I_GSM_SLEEP
# Fora de emergência: ESP32 em deep sleep, GPS sem alimentação, SIM800L em power down.
I_STANDBY = I_ESP_DEEP + I_GSM_OFF

# Antes do acionamento (repouso), por método de acionamento:
STANDBY = {
    "Button":                     I_ESP_DEEP + I_GSM_OFF,
    "Voice (160 MHz)":            I_ESP_LISTEN_160 + I_MIC + I_GSM_OFF,
    "Voice (240 MHz)":            I_ESP_LISTEN_240 + I_MIC + I_GSM_OFF,
}


def send_charge(path):
    """Carga (mA·s) e duração (s) de um envio em regime (após o 1º envio da emergência)."""
    sms = (I_ESP_AWAKE + I_GSM_TX) * T_SMS
    if path == "phone":
        q = I_ESP_BLE * T_BLE + sms
        t = T_BLE + T_SMS
    else:  # fallback: GPS ligado só durante a partida a frio, depois cortado
        q = (I_ESP_AWAKE + I_GPS_ACQ) * T_GPS_COLD + sms
        t = T_GPS_COLD + T_SMS
    return q, t

def lifetime_h(path, delta_s):
    q_act, t_act = send_charge(path)
    delta_s = np.asarray(delta_s, dtype=float)
    q_sleep = I_SLEEP * (delta_s - t_act)
    i_avg = (q_act + q_sleep) / delta_s     # mA
    return C_MAH / i_avg                    # horas

def duty(path, delta_s):
    return send_charge(path)[1] / np.asarray(delta_s, dtype=float)

if __name__ == "__main__":
    for p, name in [("phone", "celular (sem GPS)"), ("gps", "fallback (GPS a frio)")]:
        q, t = send_charge(p)
        print(f"{name:24s} carga/envio = {q/3600:.3f} mAh, acordado {t:.1f} s")
    print(f"\nFora de emergência (SIM800L em power down): {I_STANDBY:.3f} mA -> {C_MAH/I_STANDBY/24:.0f} dias"
          " (sem autodescarga da bateria)")
    # Desligar o SIM800L entre envios só compensa se registrar na rede custar menos que dormir Δ.
    for d in BAND_S:
        q_sleep_gsm = I_GSM_SLEEP * d
        print(f"Δ = {d/60:.0f} min: SIM800L em sleep gasta {q_sleep_gsm:.0f} mA·s por ciclo; desligar só compensa se o"
              f" registro levar menos de {q_sleep_gsm/(I_GSM_IDLE + I_ESP_AWAKE):.1f} s (ESP32 acordada)"
              f" ou {q_sleep_gsm/I_GSM_IDLE:.1f} s (ESP32 dormindo)")
    print()
    print(f"{'Δ':>6} | {'celular':>16} | {'fallback':>16}")
    for d in MARK_S:
        print(f"{d/60:4.0f} min | {lifetime_h('phone', d):6.0f} h ({100*duty('phone', d):4.1f}%) | "
              f"{lifetime_h('gps', d):6.0f} h ({100*duty('gps', d):4.1f}%)")

    plt.rcParams.update({"font.family": "serif", "font.serif": ["TeX Gyre Termes", "Liberation Serif", "DejaVu Serif"],
                         "font.size": 8, "axes.linewidth": 0.6})
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    x = DELTAS_S / 60
    ax.plot(x, lifetime_h("phone", DELTAS_S), color="#1f4e8c", lw=1.3, label="Smartphone path (no GNSS)")
    ax.plot(x, lifetime_h("gps", DELTAS_S), color="#b2442d", lw=1.3, ls="--", label="Fallback (GNSS cold start)")
    ax.axvline(CHOSEN_S / 60, color="0.6", lw=0.8, ls=":", zorder=0)
    for p, c, dy, va in [("phone", "#1f4e8c", 4, "bottom"), ("gps", "#b2442d", -5, "top")]:
        for d in POINTS_S:
            y = lifetime_h(p, d)
            ax.plot(d / 60, y, "o", ms=2.6, color=c, zorder=3)
            ax.annotate(f"{y:.0f}", (d / 60, y), textcoords="offset points", xytext=(0, dy),
                        ha="center", va=va, fontsize=5.8, color=c)
    ax.set_xlabel(r"Location-reporting interval $\Delta$ (min)")
    ax.set_ylabel("Battery lifetime (h)")
    ax.set_xlim(0, 10.4)
    ax.set_ylim(0, 720)
    ax.grid(True, lw=0.3, alpha=0.5)
    ax.legend(frameon=False, loc="upper left")
    fig.tight_layout(pad=0.3)
    fig.savefig("energy_lifetime.pdf")
    fig.savefig("energy_lifetime.png", dpi=200)
    print("\nFigura salva: energy_lifetime.pdf")

    # ---- Comparação dos métodos de acionamento: autonomia em repouso (antes de qualquer emergência) ----
    print("\nRepouso (esperando o acionamento):")
    for k, i in STANDBY.items():
        print(f"  {k:26s} {i:7.3f} mA -> {C_MAH / i:8.0f} h ({C_MAH / i / 24:6.1f} dias)")
    fig2, ax2 = plt.subplots(figsize=(3.45, 1.55))
    names = list(STANDBY)[::-1]
    hours = [C_MAH / STANDBY[k] for k in names]
    colors = ["#b2442d", "#d98b5f", "#1f4e8c"]
    ax2.barh(names, hours, color=colors, height=0.55)
    ax2.set_xscale("log")
    ax2.set_xlim(10, 300000)
    ax2.set_xlabel("Standby battery lifetime (h, log scale)")
    for y, h in enumerate(hours):
        label = f"{h:.0f} h" if h < 24 * 3 else f"{h:.0f} h ({h / 24:.0f} d)"
        ax2.text(h * 1.15, y, label, va="center", fontsize=6.5)
    ax2.grid(True, axis="x", lw=0.3, alpha=0.5)
    fig2.tight_layout(pad=0.3)
    fig2.savefig("standby_activation.pdf")
    fig2.savefig("standby_activation.png", dpi=200)
    print("Figura salva: standby_activation.pdf")
