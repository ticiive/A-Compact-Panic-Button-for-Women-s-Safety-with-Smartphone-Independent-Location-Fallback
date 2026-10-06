"""
Lê deepsleep_meas.csv, filtra tentativas incluídas na estatística e
imprime resumo de reconexao_ms e ack_ms, mais ACK sem a tentativa 2.
Usa apenas a biblioteca padrão do Python 3.
"""

import csv
import math
import os

CSV_PATH = os.path.join(os.path.dirname(__file__), "deepsleep_meas.csv")


def stats(values):
    n = len(values)
    if n == 0:
        return {}
    mean = sum(values) / n
    variance = sum((x - mean) ** 2 for x in values) / (n - 1) if n > 1 else 0
    sd = math.sqrt(variance)
    sorted_v = sorted(values)
    mid = n // 2
    median = sorted_v[mid] if n % 2 == 1 else (sorted_v[mid - 1] + sorted_v[mid]) / 2
    return {
        "n": n,
        "mediana": round(median),
        "media": round(mean),
        "dp": round(sd),
        "min": min(values),
        "max": max(values),
    }


def print_stats(label, s):
    print(
        f"{label}: n={s['n']}, mediana={s['mediana']}, "
        f"média={s['media']}, dp={s['dp']}, "
        f"faixa {s['min']} a {s['max']}"
    )


reconexao = []
ack = []
ack_sem_t2 = []

with open(CSV_PATH, newline="", encoding="utf-8") as f:
    reader = csv.DictReader(f)
    for row in reader:
        if row["incluida_na_estatistica"].strip() != "sim":
            continue
        tentativa = int(row["tentativa"])
        r_ms = int(row["reconexao_ms"])
        a_ms = int(row["ack_ms"])
        reconexao.append(r_ms)
        ack.append(a_ms)
        if tentativa != 2:
            ack_sem_t2.append(a_ms)

s_r = stats(reconexao)
s_a = stats(ack)
s_a2 = stats(ack_sem_t2)

print_stats("Reconexão", s_r)
print_stats("ACK (todas)", s_a)
print(
    f"ACK sem tentativa 2: n={s_a2['n']}, "
    f"média={s_a2['media']}, dp={s_a2['dp']}"
)
