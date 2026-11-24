#!/usr/bin/env python3
"""Regenerates resources/templates/builtin.json (the built-in template library).

Usage: python3 tools/gen_builtin_templates.py resources/templates/builtin.json

The JSON is the source of truth that ships in the app (embedded at build time by
cmake/modules/scene.cmake); this script only exists so the geometry of the ~30 built-in scenes
is easy to review and tweak. Note: in normalised rect units a box with w == h is 16:9 on a
16:9 frame (w*1920 : h*1080).
"""
import json
import sys


def L(role, x, y, w, h, fit="fill", radius=0, border=None, shadow=False, z=None, opacity=None):
    d = {"role": role, "slot": -1, "rect": [round(x, 4), round(y, 4), round(w, 4), round(h, 4)], "fit": fit}
    if radius:
        d["radius"] = radius
    if border:
        d["border"] = {"width": border, "color": "#ffffff"}
    if shadow:
        d["shadow"] = {"blur": 0.02, "dx": 0.004, "dy": 0.008, "opacity": 0.55}
    if z is not None:
        d["z"] = z
    if opacity is not None:
        d["opacity"] = opacity
    return d


def S(id, name, layers, bg=None, overlays=None):
    bg = bg or {"type": "solid", "color": "#0b1220"}
    s = {"id": id, "name": name, "background": bg, "layers": layers}
    if overlays:
        s["overlays"] = overlays
    return s


def T(id, name, cat, desc, scenes, roles=None):
    t = {"id": id, "name": name, "category": cat, "description": desc, "scenes": scenes}
    if roles:
        t["roles"] = roles
    return t


DARK = {"type": "gradient", "from": "#0b1220", "to": "#1c2f55", "angle": 90}
WARM = {"type": "gradient", "from": "#1a1020", "to": "#3b2a55", "angle": 45}
BLACK = {"type": "solid", "color": "#000000"}
GREEN = {"type": "gradient", "from": "#0a1f1a", "to": "#14463a", "angle": 90}


def full(role, fit="fill"):
    return L(role, 0, 0, 1, 1, fit)


def grid(roles, bg, id, name):
    n = len(roles)
    if n == 2:
        ls = [L(roles[0], 0.0, 0, 0.495, 1), L(roles[1], 0.505, 0, 0.495, 1)]
    elif n == 3:
        w = (1 - 0.02) / 3
        ls = [L(r, i * (w + 0.01), 0, w, 1) for i, r in enumerate(roles)]
    else:
        ls = [L(roles[0], 0, 0, 0.495, 0.495), L(roles[1], 0.505, 0, 0.495, 0.495),
              L(roles[2], 0, 0.505, 0.495, 0.495), L(roles[3], 0.505, 0.505, 0.495, 0.495)]
    return S(id, name, ls, bg)


def host_focus(host, others, bg, id="host-destaque", name="Host em destaque"):
    n = len(others)
    ls = [L(host, 0.0, 0, 0.7, 1)]
    h = (1 - 0.01 * (n - 1)) / n
    for i, o in enumerate(others):
        ls.append(L(o, 0.71, i * (h + 0.01), 0.29, h))
    return S(id, name, ls, bg)


templates = []

# ---------------- CULTO
templates.append(T("culto", "Culto", "culto",
  "Pregador, slides e letras de música: cinco cenas prontas para o culto.",
  [
   S("pregador", "Pregador em tela cheia", [full("Pregador")], BLACK),
   S("pregador-slides", "Pregador + slides lado a lado",
     [L("Pregador", 0.02, 0.2575, 0.47, 0.47, "fill", 0.03, 0.003, True), L("Slides", 0.51, 0.2575, 0.47, 0.47, "fit", 0.03, 0.003, True)], WARM),
   S("slides-pip", "Slides com pregador em PiP",
     [full("Slides", "fit"), L("Pregador", 0.74, 0.70, 0.24, 0.24, "fill", 0.08, 0.004, True, z=1)], BLACK),
   S("letras", "Letras de música (tela cheia)", [full("Slides", "fit")], BLACK),
   S("plano-geral", "Plano geral", [full("Plano geral")], BLACK),
  ], ["Pregador", "Slides", "Plano geral"]))

# ---------------- AULA
templates.append(T("aula", "Aula", "aula",
  "Professor e tela compartilhada: três cenas para aulas ao vivo.",
  [
   S("professor-quadro", "Professor + quadro/tela",
     [L("Tela compartilhada", 0.02, 0.19, 0.62, 0.62, "fit", 0.02, 0.003, True), L("Professor", 0.66, 0.285, 0.32, 0.32, "fill", 0.04, 0.003, True)], DARK),
   S("tela-grande-pip", "Tela compartilhada grande + professor PiP",
     [full("Tela compartilhada", "fit"), L("Professor", 0.76, 0.72, 0.22, 0.22, "fill", 0.1, 0.004, True, z=1)], BLACK),
   S("duas-colunas", "2 colunas", [L("Professor", 0.0, 0, 0.5, 1), L("Tela compartilhada", 0.5, 0, 0.5, 1, "fit")], DARK),
  ], ["Professor", "Tela compartilhada"]))

# ---------------- PODCAST
templates.append(T("podcast-2", "Podcast · 2 pessoas", "podcast",
  "Host e convidado: grade, host em tela cheia e convidado em tela cheia.",
  [grid(["Host", "Convidado"], GREEN, "grade", "Grade 2 pessoas"),
   S("host", "Host em tela cheia", [full("Host")], GREEN),
   S("convidado", "Convidado em tela cheia", [full("Convidado")], GREEN)],
  ["Host", "Convidado"]))
templates.append(T("podcast-3", "Podcast · 3 pessoas", "podcast",
  "Host e dois convidados: grade de 3 e host em destaque.",
  [grid(["Host", "Convidado 1", "Convidado 2"], GREEN, "grade", "Grade 3 pessoas"),
   host_focus("Host", ["Convidado 1", "Convidado 2"], GREEN)],
  ["Host", "Convidado 1", "Convidado 2"]))
templates.append(T("podcast-4", "Podcast · 4 pessoas", "podcast",
  "Host e três convidados: grade 2x2 e host em destaque.",
  [grid(["Host", "Convidado 1", "Convidado 2", "Convidado 3"], GREEN, "grade", "Grade 4 pessoas"),
   host_focus("Host", ["Convidado 1", "Convidado 2", "Convidado 3"], GREEN)],
  ["Host", "Convidado 1", "Convidado 2", "Convidado 3"]))
templates.append(T("podcast-entrevista", "Podcast · Entrevista", "podcast",
  "Entrevistador e entrevistado lado a lado, em cartões.",
  [S("entrevista", "Entrevista lado a lado",
     [L("Entrevistador", 0.02, 0.2575, 0.47, 0.47, "fill", 0.04, 0.003, True), L("Entrevistado", 0.51, 0.2575, 0.47, 0.47, "fill", 0.04, 0.003, True)], GREEN),
   S("entrevistador", "Entrevistador em tela cheia", [full("Entrevistador")], GREEN),
   S("entrevistado", "Entrevistado em tela cheia", [full("Entrevistado")], GREEN)],
  ["Entrevistador", "Entrevistado"]))

# ---------------- EVENTO CORPORATIVO
LT = [{"type": "lowerthird", "text": "{Palestrante}", "subtitle": "{cargo}"}]
templates.append(T("evento-palestra", "Evento · Palestra", "evento",
  "Palestrante e slides: lado a lado, slides, palestrante com lower-third.",
  [S("palestrante-slides", "Palestrante + slides",
     [L("Slides", 0.02, 0.1, 0.64, 0.64, "fit", 0.02, 0.003, True), L("Palestrante", 0.68, 0.2, 0.3, 0.3, "fill", 0.04, 0.003, True)], DARK),
   S("slides", "Slides em tela cheia", [full("Slides", "fit")], BLACK),
   S("apresentacao-lt", "Apresentação com lower-third", [full("Palestrante")], BLACK, LT)],
  ["Palestrante", "Slides"]))
templates.append(T("evento-painel", "Evento · Painel", "evento",
  "Moderador e dois painelistas: painel de 3 e moderador em destaque.",
  [grid(["Moderador", "Painelista 1", "Painelista 2"], DARK, "painel", "Painel 3 pessoas"),
   host_focus("Moderador", ["Painelista 1", "Painelista 2"], DARK, "moderador", "Moderador em destaque")],
  ["Moderador", "Painelista 1", "Painelista 2"]))
templates.append(T("evento-telas", "Evento · Abertura e encerramento", "evento",
  "Telas de abertura, intervalo e encerramento com fundo (sem câmeras).",
  [S("abertura", "Tela de abertura", [], {"type": "gradient", "from": "#0b1b3b", "to": "#2f6bff", "angle": 35},
     [{"type": "title", "text": "{evento}", "subtitle": "{data}"}]),
   S("intervalo", "Intervalo", [], {"type": "gradient", "from": "#1c2f55", "to": "#0b1220", "angle": 90},
     [{"type": "title", "text": "Voltamos já", "subtitle": ""}]),
   S("encerramento", "Tela de encerramento", [], {"type": "gradient", "from": "#2f6bff", "to": "#0b1b3b", "angle": 215},
     [{"type": "title", "text": "Obrigado!", "subtitle": "{evento}"}])],
  []))

with open(sys.argv[1], "w", encoding="utf-8") as f:
    f.write('{"format":"hb-templates","version":1,"templates":[\n')
    f.write(",\n".join(json.dumps(t, ensure_ascii=False, separators=(",", ":")) for t in templates))
    f.write("\n]}\n")
