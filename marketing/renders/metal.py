"""Steel backup render for the /metal/ page: stamped stainless washers on an M8
bolt next to a Goldfish showing the words.

Sizes are the real parts (DIN 9021 M8 washer 8.4 x 24 x 2 mm, M8 x 35 bolt,
6 mm stamps). The washers carry the words the simulator's screenshot seed shows
on the device (firmware/docs/screens/18_backup_words.png), never a real seed.

The printed box is the Knurled Blockmit Stackbox by Printer_Gobrrr
(https://makerworld.com/en/models/196485, CC BY-NC-SA), its body shortened to
45 mm for 12 washers (hardware/stackbox_12.scad, as on the /metal/ page);
assets/stackbox_*.stl are decimated copies.

Run from the repo root:
    blender -b -P marketing/renders/metal.py
Options (env): SAMPLES=256, PCT=100, CPU=1 (when the GPU is busy or too small)
Units: 1 Blender unit = 1 cm, as in device.py.
"""
import os, sys, math, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from device import *   # noqa: F401,F403  (bpy scene is reset on import)
from mathutils import Vector

if os.environ.get("CPU"):
    scene.cycles.device = 'CPU'

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf"   # plain zero, narrow block letters like stamp sets
WR, HR, WT = 1.2, 0.42, 0.2          # washer outer / hole radius, thickness
BOLT_R, PITCH = 0.4, 0.125           # M8 x 1.25
HEX_R = 1.3 / math.sqrt(3)           # 13 mm across flats -> corner radius
STAMP_H, STAMP_D = 0.6, 0.045        # 6 mm letters, stamped ~0.45 mm deep
STAMP_PITCH = 0.38                   # letter to letter along the arc, as a jig spaces them

# ---------- studio (as render.py) ----------
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

def satin(name, color, rough):
    """Stainless as washers come: matte, with fine pitting from the press."""
    m = mat(name, color, rough=rough, metal=1.0)
    nt = m.node_tree; n = nt.nodes
    noise = n.new("ShaderNodeTexNoise"); noise.inputs["Scale"].default_value = 300
    bump = n.new("ShaderNodeBump"); bump.inputs["Strength"].default_value = 0.12
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], n["Principled BSDF"].inputs["Normal"])
    return m


m_steel = satin("steel", (0.64, 0.64, 0.65, 1), 0.3)
m_stamp = satin("stamp", (0.24, 0.24, 0.25, 1), 0.6)   # the groove: rougher, darker


def threaded():
    """Stainless with a fake M8 thread: a sine bump along the object's Z."""
    m = mat("thread", (0.66, 0.66, 0.67, 1), rough=0.3, metal=1.0)
    nt = m.node_tree; n = nt.nodes
    tc = n.new("ShaderNodeTexCoord"); sep = n.new("ShaderNodeSeparateXYZ")
    mul = n.new("ShaderNodeMath"); mul.operation = 'MULTIPLY'; mul.inputs[1].default_value = 2 * math.pi / PITCH
    sin = n.new("ShaderNodeMath"); sin.operation = 'SINE'
    bump = n.new("ShaderNodeBump"); bump.inputs["Distance"].default_value = 0.03
    nt.links.new(tc.outputs["Object"], sep.inputs[0])
    nt.links.new(sep.outputs["Z"], mul.inputs[0])
    nt.links.new(mul.outputs[0], sin.inputs[0])
    nt.links.new(sin.outputs[0], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], n["Principled BSDF"].inputs["Normal"])
    return m


m_thread = threaded()


def cyl(name, r, depth, verts, material, loc=(0, 0, 0)):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=r, depth=depth, location=loc)
    o = bpy.context.object; o.name = name
    o.data.materials.append(material)
    return o


def boolean(target, cutter, op='DIFFERENCE'):
    md = target.modifiers.new("b", 'BOOLEAN'); md.operation = op; md.object = cutter; md.solver = 'MANIFOLD'   # EXACT mangled the 0
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.modifier_apply(modifier=md.name)
    bpy.data.objects.remove(cutter, do_unlink=True)


def smooth(o, bevel=0.02):
    bv = o.modifiers.new("edge", 'BEVEL'); bv.width = bevel; bv.segments = 3
    bv.limit_method = 'ANGLE'; bv.angle_limit = math.radians(40)
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.shade_auto_smooth(angle=math.radians(35))


def stamp_text(number, word, seed):
    """Hand-stamped letters, one cutter mesh each (one boolean per letter holds
    up better): the number on the top arc with its tops outward, the word on
    the bottom arc with its tops inward, so both read upright, as real stamped
    washers do. Letters sit close, each a little off in angle and depth like a
    hammer blow. Remeshed, smoothed cutters give the groove soft edges."""
    rnd = random.Random(seed)
    rc = (HR + WR) / 2                     # middle of the ring
    step = STAMP_PITCH / rc
    parts = []

    def letter(ch, a, rot):
        cu = bpy.data.curves.new("ch", 'FONT'); cu.body = ch
        cu.font = bpy.data.fonts.load(FONT, check_existing=True)
        cu.size = STAMP_H / 0.73          # cap height of DejaVu ~0.73 em
        cu.align_x = 'CENTER'; cu.align_y = 'CENTER'
        cu.extrude = 0.04                  # no bevel: DejaVu Bold's overlapping outlines break it
        o = bpy.data.objects.new("ch", cu); bpy.context.collection.objects.link(o)
        a += rnd.uniform(-0.03, 0.03)
        r = rc + rnd.uniform(-0.02, 0.02)
        depth = STAMP_D + rnd.uniform(-0.008, 0.008)
        o.location = (r * math.cos(a), r * math.sin(a), WT - depth + 0.04)
        o.rotation_euler = (rnd.uniform(-0.04, 0.04), rnd.uniform(-0.04, 0.04),
                            rot + rnd.uniform(-0.07, 0.07))
        parts.append(o)

    n = len(number)
    for i, ch in enumerate(number):        # top arc, left to right
        a = math.pi / 2 + (n - 1) / 2 * step - i * step
        letter(ch, a, a - math.pi / 2)
    n = len(word)
    for i, ch in enumerate(word):          # bottom arc, left to right
        a = 3 * math.pi / 2 - (n - 1) / 2 * step + i * step
        letter(ch, a, a + math.pi / 2)

    bpy.ops.object.select_all(action='DESELECT')
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.convert(target='MESH')
    for o in parts:
        # Text meshes aren't clean solids, and the boolean mangles some letters.
        # A voxel remesh makes each one watertight and softens it like a punch.
        rm = o.modifiers.new("rm", 'REMESH'); rm.mode = 'VOXEL'; rm.voxel_size = 0.004
        bpy.context.view_layer.objects.active = o
        bpy.ops.object.modifier_apply(modifier=rm.name)
        sm = o.modifiers.new("sm", 'SMOOTH'); sm.iterations = 4; sm.factor = 0.6
        bpy.ops.object.modifier_apply(modifier=sm.name)
        o.data.materials.clear(); o.data.materials.append(m_stamp)
    return parts


def washer(name, number=None, word=None, seed=0):
    """A DIN 9021 M8 washer lying flat, bottom at z=0; text stamped into the top."""
    w = cyl(name, WR, WT, 160, m_steel, (0, 0, WT / 2))
    boolean(w, cyl(name + "_hole", HR, 1, 96, m_steel))
    if word:
        for t in stamp_text(number, word, seed):
            # The punch pushes metal up round each letter: a low lip, made as a
            # wider, flattened copy of the letter, joined on before the cut.
            lip = t.copy(); lip.data = t.data.copy(); bpy.context.collection.objects.link(lip)
            lip.scale = (1.16, 1.16, 0.2)
            lip.location.z = WT
            lip.data.materials.clear(); lip.data.materials.append(m_steel)
            boolean(w, lip, 'UNION')
            boolean(w, t)
    smooth(w, 0.03)
    return w


def stack(name, n, loc, rot_z):
    """n washers on an M8 x 35 bolt with a nut, lying on its side."""
    root = bpy.data.objects.new(name, None); bpy.context.collection.objects.link(root)
    rnd = random.Random(name)
    head_h, nut_h, length = 0.53, 0.65, 3.5
    head = cyl(name + "_head", HEX_R, head_h, 6, m_steel, (0, 0, -head_h / 2)); smooth(head, 0.05)
    shank = cyl(name + "_shank", BOLT_R, length, 64, m_thread, (0, 0, length / 2)); smooth(shank, 0.03)
    parts = [head, shank]
    for i in range(n):
        w = washer(f"{name}_w{i}")
        # 8.4 mm holes on an 8 mm bolt: each washer sits a little off centre
        a = rnd.uniform(0, 2 * math.pi); off = rnd.uniform(0, 0.018)
        w.location = (off * math.cos(a), off * math.sin(a), i * WT)
        w.rotation_euler = (rnd.uniform(-0.01, 0.01), rnd.uniform(-0.01, 0.01), rnd.uniform(0, 2 * math.pi))
        parts.append(w)
    nut = cyl(name + "_nut", HEX_R, nut_h, 6, m_steel, (0, 0, n * WT + nut_h / 2))
    boolean(nut, cyl(name + "_nh", BOLT_R * 0.9, 2, 48, m_steel, nut.location))
    smooth(nut, 0.05); parts.append(nut)
    for p in parts:
        p.parent = root
    # bolt axis along x, resting on the washers' edges
    root.rotation_euler = (0, math.radians(90), rot_z)
    root.location = (loc[0], loc[1], WR + 0.002)
    return root


def stackbox(part, loc, rot, color):
    """A Stackbox part from assets/, standing on the floor, centred on loc."""
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.wm.stl_import(filepath=os.path.join(HERE, "assets", f"stackbox_{part}.stl"))
    o = bpy.context.selected_objects[0]
    o.scale = (0.1, 0.1, 0.1); o.rotation_euler = rot
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    bpy.ops.object.origin_set(type='ORIGIN_GEOMETRY', center='BOUNDS')
    o.location = (loc[0], loc[1], o.dimensions.z / 2 + 0.001)
    o.data.materials.append(mat(f"pla_{part}", color, rough=0.75))
    bpy.ops.object.shade_auto_smooth(angle=math.radians(30))
    return o


# ---------- the shot ----------
device("dev_a", BONE, "18_backup_words", (-4.6, 1.6, UP), (0, 0, math.radians(14)))
stack("stack", 12, (0.6, 1.4, 0), math.radians(-18))
for i, (txt, x, y, r) in enumerate([(("01", "UNVE"), 0.4, -1.9, -8), (("02", "NICE"), 3.2, -1.2, 23), (("03", "PICT"), 6.0, -0.4, -30)]):
    w = washer(f"loose{i}", *txt, seed=i)
    w.location = (x, y, 0.001)
    w.rotation_euler = (0, 0, math.radians(r))

stackbox("body", (4.4, 4.6), (math.pi, 0, 0), SIGNAL)   # open end up
stackbox("lid", (7.6, 3.0), (0, 0, 0), SIGNAL)            # open side up

scene.render.resolution_x, scene.render.resolution_y = 1600, 900
shoot("15_metal_stack.png", (1.4, -18, 17), (1.2, 1.8, 1.7), (1.2, -0.8, 0.2), lens=42)
