"""One continuous camera flight for the scroll landing page
(marketing/landing/builds/signer).

The board drops into the printed case, the cable plugs in, the device scans a
paper SeedQR card, signs a transaction, and is unplugged. Geometry is the real
case STL plus a board built from case/parts/board.gcad's dimensions; the
display shows the firmware's simulator screens.

Run from the repo root:
    blender -b -P marketing/renders/flight.py
Options (env):
    OUTDIR=/path/frames   where PNG frames go (default marketing/renders/out/flight)
    ASPECT=wide|tall      1280x720 or 720x1280 (same horizontal field of view)
    FRAMES=1,120,288      render only these frames (stills, for checking)
    STEP=1                render every Nth frame
    ENGINE=eevee|cycles   SAMPLES=64 (eevee taa / cycles samples)
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from device import *   # noqa: F401,F403
from mathutils import Vector

ASSETS = os.path.join(HERE, "assets")
OUTDIR = os.environ.get("OUTDIR", os.path.join(OUT, "flight"))
ASPECT = os.environ.get("ASPECT", "wide")
os.makedirs(OUTDIR, exist_ok=True)
END = 696


def link(ob, parent=None):
    if ob.name not in bpy.context.collection.objects:
        bpy.context.collection.objects.link(ob)
    ob.parent = parent
    return ob


def empty(name, parent=None, loc=(0, 0, 0)):
    e = bpy.data.objects.new(name, None)
    link(e, parent)
    e.location = loc
    return e


def node_mat(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    return m, m.node_tree, m.node_tree.nodes["Principled BSDF"]


def key(ob, path, frame, value, index=-1):
    setattr(ob, path, value) if index < 0 else getattr(ob, path).__setitem__(index, value)
    ob.keyframe_insert(path, frame=frame, index=index)


def smooth_keys(ob):
    ad = ob.animation_data
    if not ad or not ad.action:
        return
    for fc in _fcurves(ad):
        for kp in fc.keyframe_points:
            kp.interpolation = 'BEZIER'
            kp.handle_left_type = kp.handle_right_type = 'AUTO_CLAMPED'


def _fcurves(ad):
    act = ad.action
    if hasattr(act, "fcurves") and len(getattr(act, "fcurves", [])):
        return act.fcurves
    out = []   # Blender 5: layered actions
    for layer in getattr(act, "layers", []):
        for strip in layer.strips:
            for bag in strip.channelbags:
                out.extend(bag.fcurves)
    return out


def hard_keys(ob):
    ad = ob.animation_data
    for fc in _fcurves(ad):
        for kp in fc.keyframe_points:
            kp.interpolation = 'CONSTANT'


# ------------------------------------------------------------------ paper
def paper_mat():
    m, nt, p = node_mat("paper")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    fib = nt.nodes.new("ShaderNodeTexNoise"); fib.inputs["Scale"].default_value = 45.0
    fib.inputs["Detail"].default_value = 10.0
    mp = nt.nodes.new("ShaderNodeMapping"); mp.inputs["Scale"].default_value = (1.0, 6.0, 1.0)
    streak = nt.nodes.new("ShaderNodeTexNoise"); streak.inputs["Scale"].default_value = 2.5
    broad = nt.nodes.new("ShaderNodeTexNoise"); broad.inputs["Scale"].default_value = 0.05
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.80, 0.75, 0.64, 1)
    ramp.color_ramp.elements[1].color = (0.86, 0.81, 0.70, 1)
    bump = nt.nodes.new("ShaderNodeBump"); bump.inputs["Strength"].default_value = 0.06
    mix = nt.nodes.new("ShaderNodeMix"); mix.data_type = 'FLOAT'; mix.inputs["Factor"].default_value = 0.3
    nt.links.new(tc.outputs["Object"], fib.inputs["Vector"])
    nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], streak.inputs["Vector"])
    nt.links.new(tc.outputs["Object"], broad.inputs["Vector"])
    nt.links.new(broad.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], p.inputs["Base Color"])
    nt.links.new(fib.outputs["Fac"], mix.inputs["A"])
    nt.links.new(streak.outputs["Fac"], mix.inputs["B"])
    nt.links.new(mix.outputs["Result"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], p.inputs["Normal"])
    p.inputs["Roughness"].default_value = 0.92
    p.inputs["Specular IOR Level"].default_value = 0.15
    return m


w = bpy.data.worlds.new("w"); scene.world = w
w.use_nodes = True
w.node_tree.nodes["Background"].inputs["Color"].default_value = (0.80, 0.75, 0.64, 1)
w.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.45

bpy.ops.mesh.primitive_plane_add(size=600, location=(0, 0, 0))
bpy.context.object.data.materials.append(paper_mat())


def area(name, loc, size, power, color=(1, 0.97, 0.92), target=(0, 0, 0)):
    l = bpy.data.lights.new(name, 'AREA'); l.size = size; l.energy = power; l.color = color
    o = bpy.data.objects.new(name, l); link(o)
    o.location = loc
    o.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()


area("key", (-30, -10, 45), 30, 52000)
area("rim", (30, 35, 25), 18, 14000, (0.9, 0.95, 1.0))
area("fill", (35, -30, 18), 20, 6000)

# ------------------------------------------------------------------ device
RZ = math.radians(-12)
dev = empty("dev")
dev.location = (0, 0, UP)
dev.rotation_euler = (0, 0, RZ)

shell = CASE_SRC.copy(); link(shell, dev)
shell.hide_render = False; shell.hide_viewport = False
if not shell.material_slots:
    shell.data.materials.append(None)
shell.material_slots[0].link = 'OBJECT'
shell.material_slots[0].material = mat("pla", GRAPHITE, rough=0.62)

# the board: Waveshare RP2350-Touch-LCD-2, dimensions from case/parts/board.gcad
board = empty("board", dev)
m_pcb = mat("pcb", (0.03, 0.12, 0.42, 1), rough=0.45, coat=0.4)
m_pin = mat("pin", (0.75, 0.6, 0.25, 1), rough=0.3, metal=1.0)
m_white = mat("conn", (0.8, 0.78, 0.72, 1), rough=0.5)
rounded_box("b_glass", GW, GH, 0.06, GR, 0.015, m_glass, board, (0, 0, -0.03))
rounded_box("b_lcd", 3.6, 5.8, 0.50, 0.2, 0.01, m_dark, board, (0, 0, -0.31))
rounded_box("b_pcb", 3.5, 4.82, 0.16, 0.1, 0.01, m_pcb, board, (0, 0, -0.64))
for sx in (1, -1):
    rounded_box(f"b_hdr{sx}", 0.254, 3.5, 0.25, 0.02, 0.0, m_dark, board, (sx * 1.524, 0, -0.845))
    for i in range(14):
        rounded_box(f"b_pin{sx}_{i}", 0.064, 0.064, 0.55, 0.0, 0.0, m_pin, board,
                    (sx * 1.524, -1.65 + i * 0.254, -1.0))
for bx, by in ((1.45, 2.11), (-1.45, 2.11), (1.45, -2.11), (-1.45, -2.11)):
    bpy.ops.mesh.primitive_cylinder_add(radius=0.17, depth=0.25, vertices=24, location=(bx, by, -0.845))
    o = bpy.context.object; o.parent = board; o.data.materials.append(m_pin)
rounded_box("b_usb", 0.894, 0.735, 0.326, 0.15, 0.0, m_metal, board, (0, -2.04, -0.883))
rounded_box("b_usbin", 0.80, 0.02, 0.24, 0.11, 0.0, m_dark, board, (0, -2.415, -0.883))
rounded_box("b_bat", 0.81, 0.44, 0.45, 0.03, 0.0, m_white, board, (-0.805, -0.11, -0.945))
rounded_box("b_fpc", 1.51, 0.37, 0.2, 0.02, 0.0, m_dark, board, (0.085, -1.005, -0.82))
rounded_box("b_cam", 0.85, 0.85, 0.4, 0.05, 0.0, m_dark, board, (0, CAMY, -1.32))
bpy.ops.mesh.primitive_cylinder_add(radius=0.3, depth=0.14, vertices=96, location=(0, CAMY, ZB + 0.12))
o = bpy.context.object; o.parent = board; o.data.materials.append(m_dark); bpy.ops.object.shade_smooth()
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.2, segments=64, ring_count=32, location=(0, CAMY, ZB + 0.06))
o = bpy.context.object; o.scale = (1, 1, 0.3); o.parent = board
o.data.materials.append(m_lens); bpy.ops.object.shade_smooth()

# one display plane per screen; visibility is keyframed
SCREENS_AT = [  # (frame it appears, screen)
    (300, "1_home"), (336, "2_scan_waiting"), (392, "3_seed_loaded"),
    (452, "7_psbt_overview"), (500, "10_psbt_sign"), (540, "11_psbt_signed"),
]
OFF_AT = 612
displays = []
for i, (_, name) in enumerate(SCREENS_AT):
    bpy.ops.mesh.primitive_plane_add(size=1, location=(0, SCY, 0.001 + i * 0.0002))
    d = bpy.context.object; d.scale = (SW, SH, 1); d.parent = board
    d.data.materials.append(screen_mat(name))
    displays.append(d)
for i, d in enumerate(displays):
    start = SCREENS_AT[i][0]
    stop = SCREENS_AT[i + 1][0] if i + 1 < len(SCREENS_AT) else OFF_AT
    for f, hidden in ((1, True), (start, False), (stop, True)):
        d.hide_render = hidden; d.keyframe_insert("hide_render", frame=f)
        d.hide_viewport = hidden; d.keyframe_insert("hide_viewport", frame=f)
    hard_keys(d)

# board drop: floats tilted with its back to the camera, settles flat into the case
for f, z, rx, ry, rz in (
    (1, 7.5, -118, 8, 38),
    (120, 6.8, -70, 6, 22),
    (200, 3.2, -14, 2, 6),
    (246, 0.25, 0, 0, 0),
    (256, 0.0, 0, 0, 0),
):
    key(board, "location", f, (0, 0, z))
    key(board, "rotation_euler", f, (math.radians(rx), math.radians(ry), math.radians(rz)))
smooth_keys(board)

# ------------------------------------------------------------------ card and cable
def seed_card(loc, rz):
    m, nt, p = node_mat("seedqr_card")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(os.path.join(ASSETS, "seedqr_card.png"))
    tex.extension = 'EXTEND'
    nt.links.new(tc.outputs["UV"], tex.inputs["Vector"])
    nt.links.new(tex.outputs["Color"], p.inputs["Base Color"])
    p.inputs["Roughness"].default_value = 0.85
    p.inputs["Specular IOR Level"].default_value = 0.2
    bpy.ops.mesh.primitive_plane_add(size=1)
    ob = bpy.context.object
    ob.scale = (8.56, 5.4, 1)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bev = ob.modifiers.new("corners", 'BEVEL'); bev.affect = 'VERTICES'; bev.width = 0.3; bev.segments = 8
    ob.data.materials.append(m)
    ob.location = (loc[0], loc[1], 0.02); ob.rotation_euler = (0, 0, rz)
    return ob


CARD = (-9.5, 1.2)
seed_card(CARD, math.radians(6))

m_cable = mat("cable", (0.02, 0.02, 0.02, 1), rough=0.55)
m_plug = mat("plug", (0.025, 0.025, 0.025, 1), rough=0.45)
plug = empty("plug", dev)
rounded_box("plug_body", 1.15, 2.2, 0.62, 0.3, 0.12, m_plug, plug, (0, -3.13 - 1.0, -0.88))
rounded_box("plug_tip", 0.84, 0.9, 0.26, 0.12, 0.0, m_metal, plug, (0, -2.6, -0.88))
for f, y in ((1, -9.0), (258, -9.0), (284, 0.0), (598, 0.0), (624, -3.2)):
    key(plug, "location", f, (0, y, 0))
smooth_keys(plug)

# cable: first two points ride the plug (hook), the rest lie on the paper
cu = bpy.data.curves.new("cable", 'CURVE'); cu.dimensions = '3D'
cu.bevel_depth = 0.17; cu.bevel_resolution = 6
sp = cu.splines.new('BEZIER')
local = [Vector((0, -5.23, -0.88)), Vector((0, -7.0, -1.3))]
bpy.context.view_layer.update()
dev_m = dev.matrix_world.copy()
pts = [dev_m @ p for p in local] + [Vector(v) for v in ((-2.6, -10.5, 0.17), (-2.0, -15, 0.17), (1.5, -22, 0.17), (3, -40, 0.17))]
sp.bezier_points.add(len(pts) - 1)
for bp, co in zip(sp.bezier_points, pts):
    bp.co = co; bp.handle_left_type = bp.handle_right_type = 'AUTO'
cable = bpy.data.objects.new("cable", cu); link(cable)
cu.materials.append(m_cable)
hook_e = empty("cable_hook", plug)
bpy.context.view_layer.update()
hk = cable.modifiers.new("hook", 'HOOK')
hk.object = hook_e
hk.vertex_indices_set([0, 1, 2, 3, 4, 5])
hk.matrix_inverse = hook_e.matrix_world.inverted()

# device lifts, glides over the card to scan it, comes back
cx, cy = CARD
for f, loc in (
    (1, (0, 0, UP)), (340, (0, 0, UP)), (364, (cx * 0.55, cy * 0.5, UP + 2.6)),
    (380, (cx + 0.6, cy + 0.2, UP + 2.4)), (404, (cx + 0.6, cy + 0.2, UP + 2.4)),
    (430, (cx * 0.35, cy * 0.3, UP + 1.4)), (446, (0, 0, UP)),
):
    key(dev, "location", f, loc)
smooth_keys(dev)

# ------------------------------------------------------------------ camera
target = empty("target")
cam_data.dof.use_dof = True
cam_data.dof.focus_object = target
cam_data.dof.aperture_fstop = 5.6
cam_data.sensor_fit = 'HORIZONTAL'
con = cam.constraints.new('TRACK_TO'); con.target = target
con.track_axis = 'TRACK_NEGATIVE_Z'; con.up_axis = 'UP_Y'

SHOTS = [  # frame, camera, target, lens, x offset for the wide frame's copy space
    (1,   (5.2, -17.0, 22.0), (0.0, 0.0, 8.0), 50, -3.2),
    (120, (7.9, -19.5, 23.5), (0.0, 0.3, 6.8), 50, -2.4),
    (210, (9.0, -19.0, 21.0), (0.0, 0.2, 3.0), 46, 0),
    (288, (8.0, -18.5, 22.0), (-0.6, 0.4, 1.4), 46, 0),
    (360, (2.0, -19.0, 21.0), (-4.6, 0.7, 2.6), 44, 0),
    (404, (-2.5, -17.0, 20.0), (-8.6, 1.2, 2.6), 46, 0),
    (452, (1.2, -12.0, 19.0), (0.0, 0.2, 1.4), 50, 0),
    (520, (0.8, -6.0, 17.0), (0.0, 0.3, 1.7), 50, 0),
    (576, (0.8, -6.5, 17.5), (0.0, 0.3, 1.7), 50, 0),
    (640, (-3.0, -3.5, 40.0), (-3.0, -1.6, 0.0), 40, 6.6),
    (696, (-2.9, -3.6, 40.5), (-2.9, -1.7, 0.0), 40, 6.6),
]
for f, c, t, lens, off in SHOTS:
    off = off if ASPECT != "tall" else 0
    key(cam, "location", f, (c[0] + off, c[1], c[2]))
    key(target, "location", f, (t[0] + off, t[1], t[2]))
    cam_data.lens = lens; cam_data.keyframe_insert("lens", frame=f)
smooth_keys(cam); smooth_keys(target)
for fc in _fcurves(cam_data.animation_data):
    for kp in fc.keyframe_points:
        kp.handle_left_type = kp.handle_right_type = 'AUTO_CLAMPED'

# ------------------------------------------------------------------ render
engine = os.environ.get("ENGINE", "eevee")
samples = int(os.environ.get("SAMPLES", 64))
if engine == "eevee":
    for name in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            scene.render.engine = name; break
        except TypeError:
            continue
    ee = scene.eevee
    ee.taa_render_samples = samples
    for attr in ("use_raytracing", "use_shadows", "use_gtao"):
        if hasattr(ee, attr):
            setattr(ee, attr, True)
else:
    scene.cycles.samples = samples
if ASPECT == "tall":
    scene.render.resolution_x, scene.render.resolution_y = 720, 1280
    cam_data.sensor_fit = 'VERTICAL'
    cam_data.sensor_height = 36 * 0.8
else:
    scene.render.resolution_x, scene.render.resolution_y = 1280, 720
scene.render.resolution_percentage = int(os.environ.get("PCT", 100))
scene.frame_start, scene.frame_end = 1, END
scene.render.fps = 24
scene.render.image_settings.file_format = 'PNG'

frames = os.environ.get("FRAMES")
step = int(os.environ.get("STEP", 1))
todo = [int(f) for f in frames.split(",")] if frames else list(range(1, END + 1, step))
for f in todo:
    scene.frame_set(f)
    scene.render.filepath = os.path.join(OUTDIR, f"f{f:04d}.png")
    bpy.ops.render.render(write_still=True)
    print("wrote", scene.render.filepath, flush=True)
