"""Product renders of Goldfish, the stateless signer (studio shots).

Uses the real printed case (case/exports/case.stl) and the firmware's actual
screens (firmware/docs/screens/*.png), so the pictures show what the device
really looks like. Lifestyle scenes live in scenes.py.

Run from the repo root:
    blender -b -P marketing/renders/render.py
Options (env): SAMPLES=256, PCT=100, ONLY=1,3 (shot numbers)
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from device import *   # noqa: F401,F403  (bpy scene is reset on import)

# ---------- studio ----------
world = bpy.data.worlds.new("w"); scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs["Color"].default_value = (0.60, 0.58, 0.55, 1)
bg.inputs["Strength"].default_value = 0.35

bpy.ops.mesh.primitive_plane_add(size=400, location=(0, 0, 0))
bpy.context.object.data.materials.append(mat("floor", (0.60, 0.58, 0.55, 1), rough=0.8))


def area(name, loc, size, power, color=(1, 0.97, 0.92)):
    l = bpy.data.lights.new(name, 'AREA'); l.size = size; l.energy = power; l.color = color
    o = bpy.data.objects.new(name, l); bpy.context.collection.objects.link(o)
    o.location = loc
    o.rotation_euler = (Vector((0, 0, 0)) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()


area("key", (-30, -25, 45), 30, 52000)
area("rim", (35, 30, 20), 15, 15000, (0.9, 0.95, 1.0))
area("fill", (40, -30, 15), 20, 5000)

ONLY = os.environ.get("ONLY", "")


def clear_devices():
    for o in [o for o in bpy.data.objects
              if o.name.startswith("dev") or (o.parent and o.parent.name.startswith("dev"))]:
        bpy.data.objects.remove(o, do_unlink=True)


def want(n):
    return not ONLY or n in ONLY.split(",")


if want("1"):   # hero: reviewing a transaction
    device("dev_a", GRAPHITE, "7_psbt_overview", (0, 0, UP), (0, 0, math.radians(22)))
    shoot("01_hero_review.png", (9, -16, 15), (0, 0.2, 0.8), (0, 0.3, 1.7))
    clear_devices()
if want("2"):   # back: camera
    device("dev_b", GRAPHITE, "4_seed_menu", (0, 0, DOWN), (0, math.pi, math.radians(15)))
    shoot("02_back.png", (-10, -15, 12), (0, 0.2, 0.6), (0, 0.5, 1.7))
    clear_devices()
if want("3"):   # colourways across the flow
    device("dev_c", GRAPHITE, "3_seed_loaded", (-7.8, 0.2, UP), (0, 0, math.radians(5)))
    device("dev_d", SIGNAL, "10_psbt_sign", (-2.6, -0.2, UP), (0, 0, math.radians(1)))
    device("dev_d2", BLUE, "11_psbt_signed", (2.6, -0.2, UP), (0, 0, math.radians(-1)))
    device("dev_e", BONE, "13_verify_found", (7.8, 0.2, UP), (0, 0, math.radians(-5)))
    shoot("03_colorways.png", (0, -20, 40), (0, 0.6, 0.8), (0, 0, 1.7), lens=78)
    clear_devices()
if want("4"):   # edge: USB-C and buttons
    device("dev_f", SIGNAL, "13_verify_found", (0, 0, UP), (0, 0, math.radians(35)))
    shoot("04_edge.png", (13, -20, 6), (0, -0.6, 0.9), (1.2, -1.8, 0.9), lens=100)
    clear_devices()
if want("5"):   # top-down wallet menu, close
    device("dev_g", BONE, "4_seed_menu", (0, 0, UP), (0, 0, math.radians(-8)))
    shoot("05_wallet_topdown.png", (2.5, -10, 21), (0, 0.4, 0.8), (0, 0.2, 1.7), lens=85)
    clear_devices()
if want("6"):   # lineup: start, new seed, wallet menu, connect wallet
    device("dev_h", GRAPHITE, "1_home", (-7.8, 0.2, UP), (0, 0, math.radians(4)))
    device("dev_i", BONE, "15_new_seed", (-2.6, -0.2, UP), (0, 0, math.radians(1)))
    device("dev_j", SIGNAL, "4_seed_menu", (2.6, -0.2, UP), (0, 0, math.radians(-1)))
    device("dev_k", BLUE, "5_export_xpub", (7.8, 0.2, UP), (0, 0, math.radians(-4)))
    shoot("06_lineup.png", (0, -20, 40), (0, 0.6, 0.8), (0, 0, 1.7), lens=78)
    clear_devices()
if want("7"):   # hero: start screen, whole device in view
    device("dev_l", GRAPHITE, "1_home", (0, 0, UP), (0, 0, math.radians(18)))
    shoot("07_hero_home.png", (7, -15, 17), (0.3, -0.6, 0.9), (0, 0.4, 1.7), lens=70)
    clear_devices()
if want("8"):   # slide to sign, close
    device("dev_m", SIGNAL, "10_psbt_sign", (0, 0, UP), (0, 0, math.radians(-14)))
    shoot("08_slide_to_sign.png", (-6, -14, 18), (0.4, -0.4, 0.9), (0, 0.4, 1.7), lens=75)
    clear_devices()
if want("9"):   # new seed from dice rolls, close
    device("dev_n", BONE, "16_dice", (0, 0, UP), (0, 0, math.radians(14)))
    shoot("09_new_seed_dice.png", (6, -14, 18), (-0.2, -0.4, 0.9), (0, 0.4, 1.7), lens=75)
    clear_devices()
if want("10"):  # the signing flow, three devices filling the frame
    device("dev_o", GRAPHITE, "3_seed_loaded", (-4.6, 0.4, UP), (0, 0, math.radians(5)))
    device("dev_p", SIGNAL, "10_psbt_sign", (0, 0.1, UP), (0, 0, 0))
    device("dev_q", BONE, "11_psbt_signed", (4.6, 0.4, UP), (0, 0, math.radians(-5)))
    shoot("14_sign_flow.png", (0, -14, 28), (0, -0.2, 0.8), (0, 0, 1.7), lens=66)
    clear_devices()
