"""Turntable frames for the landing page's hero: the device standing upright,
screen to the viewer, USB-C down, turned a full circle about its vertical axis,
once per case colour. Transparent background; the page adds the shadow.

Run from the repo root:
    blender -b -P marketing/renders/turntable.py
    uv run --with pillow python marketing/renders/turntable_pack.py   # -> landing/img/turn/
Options (env): SAMPLES=64, FRAMES=36, COLORS=orange,black,blue,white, ONLY_FRAME=0
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from device import *   # noqa: F401,F403  (bpy scene is reset on import)

FRAMES = int(os.environ.get("FRAMES", 36))
COLORS = {"orange": SIGNAL, "black": GRAPHITE, "blue": BLUE, "white": BONE}   # names as in landing/brand.css
WANT = os.environ.get("COLORS", ",".join(COLORS)).split(",")
ONLY_FRAME = os.environ.get("ONLY_FRAME")
TT_OUT = os.path.join(OUT, "turntable")

# The home screen without its version in the corner: the frames outlive the build
# they were rendered from (the page's flat screenshots get the release's tag on deploy).
os.makedirs(TT_OUT, exist_ok=True)
_src = bpy.data.images.load(os.path.join(SCREENS, "1_home.png"))
_w, _h = _src.size
_px = list(_src.pixels)
_bg = _px[((_h - 1 - 12) * _w + 140) * 4:][:4]
for y in range(8, 21):                     # version text: x 173-230, y 10-18 (from the top)
    for x in range(170, 234):
        i = ((_h - 1 - y) * _w + x) * 4
        _px[i:i + 4] = _bg
_src.pixels = _px
_src.filepath_raw = os.path.join(TT_OUT, "1_home.png"); _src.file_format = 'PNG'; _src.save()
HOME = os.path.relpath(os.path.join(TT_OUT, "1_home"), SCREENS)

world = bpy.data.worlds.new("w"); scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs["Color"].default_value = (1, 1, 1, 1)
bg.inputs["Strength"].default_value = 0.12   # low, so the black glass stays black
scene.render.film_transparent = True
# Standard, not the studio shots' AgX: AgX's curve greys the screen's white and
# washes its orange out, and the page lays the live firmware over the front frame
scene.view_settings.view_transform = 'Standard'


def area(name, loc, size, power, color=(1, 0.98, 0.95)):
    l = bpy.data.lights.new(name, 'AREA'); l.size = size; l.energy = power; l.color = color
    o = bpy.data.objects.new(name, l); bpy.context.collection.objects.link(o)
    o.location = loc
    o.rotation_euler = (Vector((0, 0, 0)) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()


area("key", (-25, -30, 35), 25, 30000)
area("rim", (30, 25, 20), 15, 12000, (0.92, 0.96, 1.0))
area("fill", (30, -30, 5), 20, 6000)

# The pivot turns; the device stands on it: glass (+Z) to -Y, USB (-Y) down,
# moved so its middle (half the case depth behind the glass) is on the axis.
pivot = bpy.data.objects.new("pivot", None); bpy.context.collection.objects.link(pivot)

scene.render.resolution_x, scene.render.resolution_y = 480, 640
scene.render.resolution_percentage = 100
scene.cycles.samples = int(os.environ.get("SAMPLES", 64))
scene.render.image_settings.file_format = 'PNG'
scene.render.image_settings.color_mode = 'RGBA'

cam_loc = (0, -60, 4.5)          # a touch above the middle, long lens: nearly flat
cam.location = cam_loc; cam_data.lens = 300
cam.rotation_euler = (Vector((0, 0, 0)) - Vector(cam_loc)).to_track_quat('-Z', 'Y').to_euler()
cam_data.dof.use_dof = False

for cname in WANT:
    clear_devices()
    dev = device("dev_" + cname, COLORS[cname], HOME, (0, (ZB - ZT) / 2, 0), (math.pi / 2, 0, 0))
    dev.parent = pivot
    # with Standard, ~0.9 shows the screenshot's own white (~245), as the live canvas does
    screen_mat(HOME).node_tree.nodes["Principled BSDF"].inputs["Emission Strength"].default_value = 0.9
    os.makedirs(os.path.join(TT_OUT, cname), exist_ok=True)
    frames = [int(ONLY_FRAME)] if ONLY_FRAME else range(FRAMES)
    for i in frames:
        pivot.rotation_euler = (0, 0, 2 * math.pi * i / FRAMES)
        scene.render.filepath = os.path.join(TT_OUT, cname, f"{i:02d}.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", scene.render.filepath)
