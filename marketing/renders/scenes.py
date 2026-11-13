"""Lifestyle mockups: the real case in context (desk with a laptop, flat lay
with a paper SeedQR backup, close-up). Device geometry and screens come from
device.py, i.e. the real case STL and the firmware's simulator screenshots.

The laptop shows a generic wallet window (assets/laptop_screen.png), not any
real app's UI. The paper SeedQR card carries SeedSigner's public 12-word test vector
(the same seed the device screens show, fingerprint 0948d268).

Run from the repo root:
    blender -b -P marketing/renders/scenes.py
Options (env): SAMPLES=256, PCT=100, ONLY=a,b,c,d
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from device import *   # noqa: F401,F403
from mathutils import Vector

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
ONLY = os.environ.get("ONLY", "")


def want(n):
    return not ONLY or n in ONLY.split(",")


def link(ob, parent=None):
    if ob.name not in bpy.context.collection.objects:
        bpy.context.collection.objects.link(ob)
    ob.parent = parent
    return ob


def node_mat(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    return m, m.node_tree, m.node_tree.nodes["Principled BSDF"]


# ---------------------------------------------------------------- surfaces
def wood_mat():
    m, nt, p = node_mat("wood")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (0.35, 0.05, 0.35)   # grain runs along Y
    wave = nt.nodes.new("ShaderNodeTexWave")
    wave.wave_type = 'BANDS'; wave.bands_direction = 'X'
    wave.inputs["Scale"].default_value = 1.6
    wave.inputs["Distortion"].default_value = 3.0
    wave.inputs["Detail"].default_value = 3.0
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 8.0
    mix = nt.nodes.new("ShaderNodeMix"); mix.data_type = 'FLOAT'
    mix.inputs["Factor"].default_value = 0.25
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.28, 0.16, 0.08, 1)
    ramp.color_ramp.elements[1].color = (0.52, 0.34, 0.19, 1)
    nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], wave.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], noise.inputs["Vector"])
    nt.links.new(wave.outputs["Fac"], mix.inputs["A"])
    nt.links.new(noise.outputs["Fac"], mix.inputs["B"])
    nt.links.new(mix.outputs["Result"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], p.inputs["Base Color"])
    p.inputs["Roughness"].default_value = 0.45
    p.inputs["Coat Weight"].default_value = 0.25
    return m


def linen_mat():
    m, nt, p = node_mat("linen")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    fine = nt.nodes.new("ShaderNodeTexNoise"); fine.inputs["Scale"].default_value = 60.0
    fine.inputs["Detail"].default_value = 8.0
    broad = nt.nodes.new("ShaderNodeTexNoise"); broad.inputs["Scale"].default_value = 0.08
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.70, 0.67, 0.62, 1)
    ramp.color_ramp.elements[1].color = (0.80, 0.77, 0.72, 1)
    bump = nt.nodes.new("ShaderNodeBump"); bump.inputs["Strength"].default_value = 0.08
    nt.links.new(tc.outputs["Object"], fine.inputs["Vector"])
    nt.links.new(tc.outputs["Object"], broad.inputs["Vector"])
    nt.links.new(broad.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], p.inputs["Base Color"])
    nt.links.new(fine.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], p.inputs["Normal"])
    p.inputs["Roughness"].default_value = 0.9
    return m


def slate_mat():
    m, nt, p = node_mat("slate")
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 3.0
    noise.inputs["Detail"].default_value = 12.0
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.045, 0.047, 0.05, 1)
    ramp.color_ramp.elements[1].color = (0.085, 0.088, 0.092, 1)
    bump = nt.nodes.new("ShaderNodeBump"); bump.inputs["Strength"].default_value = 0.15
    nt.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], p.inputs["Base Color"])
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], p.inputs["Normal"])
    p.inputs["Roughness"].default_value = 0.85
    return m


def ground(material):
    bpy.ops.mesh.primitive_plane_add(size=300, location=(0, 0, 0))
    g = bpy.context.object
    g.name = "ground"
    g.data.materials.append(material)
    return g


# ---------------------------------------------------------------- props
m_alu = mat("alu", (0.72, 0.72, 0.74, 1), rough=0.32, metal=1.0)
m_keys = mat("keys", (0.03, 0.03, 0.035, 1), rough=0.6)
m_bezel = mat("bezel", (0.01, 0.01, 0.012, 1), rough=0.1, coat=1.0)
m_cable = mat("cable", (0.02, 0.02, 0.02, 1), rough=0.55)
m_plug = mat("plug", (0.025, 0.025, 0.025, 1), rough=0.45)


def emissive_image(name, path, strength=1.4, gloss=1.0):
    m, nt, p = node_mat(name)
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(path)
    p.inputs["Base Color"].default_value = (0, 0, 0, 1)
    p.inputs["Roughness"].default_value = 0.08 if gloss else 0.6
    p.inputs["Coat Weight"].default_value = gloss
    p.inputs["Specular IOR Level"].default_value = 0.5 if gloss else 0.05
    p.inputs["Emission Strength"].default_value = strength
    nt.links.new(tex.outputs["Color"], p.inputs["Emission Color"])
    return m


def laptop(loc, rz, open_deg=112):
    root = bpy.data.objects.new("laptop", None)
    link(root)
    W, D = 30.4, 21.2
    rounded_box("lt_base", W, D, 1.3, 1.0, 0.25, m_alu, root, (0, 0, 0.65))
    rounded_box("lt_keys", 26.5, 10.5, 0.05, 0.4, 0.0, m_keys, root, (0, 2.2, 1.31))
    rounded_box("lt_pad", 11.5, 7.0, 0.04, 0.4, 0.0, mat("pad", (0.6, 0.6, 0.62, 1), rough=0.25, metal=1.0),
                root, (0, -6.0, 1.31))
    hinge = bpy.data.objects.new("lt_hinge", None)
    link(hinge, root)
    hinge.location = (0, D / 2, 1.3)
    hinge.rotation_euler = (-math.radians(open_deg), 0, 0)
    rounded_box("lt_lid", W, D - 0.4, 0.5, 1.0, 0.2, m_alu, hinge, (0, -(D - 0.4) / 2, 0.25))
    rounded_box("lt_glass", W - 0.6, D - 1.0, 0.04, 0.8, 0.0, mat("lt_bezel", (0.01, 0.01, 0.012, 1), rough=0.5), hinge,
                (0, -(D - 0.4) / 2, -0.03))
    bpy.ops.mesh.primitive_plane_add(size=1)
    scr = bpy.context.object
    scr.scale = (27.6, 17.25, 1)
    scr.location = (0, -(D - 0.4) / 2 - 0.4, -0.055)
    scr.rotation_euler = (math.pi, 0, 0)             # screen faces out of the lid's inner side
    scr.data.materials.append(emissive_image("lt_screen", os.path.join(ASSETS, "laptop_screen.png"), 1.6, gloss=0.0))
    link(scr, hinge)
    root.location = loc
    root.rotation_euler = (0, 0, rz)
    return root


def seed_card(loc, rz):
    """Paper SeedQR backup card, credit-card size, filled in by hand (assets/seedqr_card.png)."""
    m, nt, p = node_mat("seedqr_card")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(os.path.join(ASSETS, "seedqr_card.png"))
    tex.extension = 'EXTEND'
    nt.links.new(tc.outputs["UV"], tex.inputs["Vector"])
    nt.links.new(tex.outputs["Color"], p.inputs["Base Color"])
    noise = nt.nodes.new("ShaderNodeTexNoise"); noise.inputs["Scale"].default_value = 300.0
    bump = nt.nodes.new("ShaderNodeBump"); bump.inputs["Strength"].default_value = 0.05
    nt.links.new(noise.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], p.inputs["Normal"])
    p.inputs["Roughness"].default_value = 0.85
    p.inputs["Specular IOR Level"].default_value = 0.2

    bpy.ops.mesh.primitive_plane_add(size=1)
    ob = bpy.context.object
    ob.name = "seedqr_card"
    ob.scale = (8.56, 5.4, 1)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bev = ob.modifiers.new("corners", 'BEVEL')
    bev.affect = 'VERTICES'; bev.width = 0.3; bev.segments = 8
    ob.data.materials.append(m)
    ob.location = (loc[0], loc[1], 0.02)
    ob.rotation_euler = (0, 0, rz)
    return ob


def usb_cable(dev_root, path_world):
    """Plug at the device's USB-C cutout, cable following path_world (XY points on the ground)."""
    # plug overmold sticking out of the bottom wall (device-local coords, cm)
    rounded_box(dev_root.name + "_plug", 1.15, 2.2, 0.62, 0.3, 0.12, m_plug, dev_root, (0, -3.13 - 1.0, -0.88))
    dev_root.matrix_world.translation  # ensure matrices are current
    bpy.context.view_layer.update()
    start = dev_root.matrix_world @ Vector((0, -3.13 - 2.1, -0.88))
    cu = bpy.data.curves.new("cable", 'CURVE')
    cu.dimensions = '3D'
    cu.bevel_depth = 0.17
    cu.bevel_resolution = 6
    sp = cu.splines.new('BEZIER')
    pts = [start] + [Vector((x, y, 0.17)) for x, y in path_world]
    sp.bezier_points.add(len(pts) - 1)
    for bp, co in zip(sp.bezier_points, pts):
        bp.co = co
        bp.handle_left_type = bp.handle_right_type = 'AUTO'
    ob = bpy.data.objects.new("cable", cu)
    link(ob)
    cu.materials.append(m_cable)
    return ob


def area(name, loc, size, power, color=(1, 0.97, 0.92), target=(0, 0, 0)):
    l = bpy.data.lights.new(name, 'AREA'); l.size = size; l.energy = power; l.color = color
    o = bpy.data.objects.new(name, l); link(o)
    o.location = loc
    o.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    return o


def clear_scene():
    keep = {"cam"}
    for o in list(bpy.data.objects):
        if o.name in keep or o.hide_render and o.type == 'MESH':
            continue
        bpy.data.objects.remove(o, do_unlink=True)


def world(color, strength):
    w = bpy.data.worlds.new("w"); scene.world = w
    w.use_nodes = True
    bg = w.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = color
    bg.inputs["Strength"].default_value = strength


def top_down(fname, xy, height, lens=50, rz=0.0):
    cam.location = (xy[0], xy[1], height)
    cam.rotation_euler = (0, 0, rz)
    cam_data.lens = lens
    cam_data.dof.focus_distance = height - 1.7
    cam_data.dof.aperture_fstop = 8.0
    scene.render.filepath = os.path.join(OUT, fname)
    bpy.ops.render.render(write_still=True)
    print("wrote", scene.render.filepath)


# ---------------------------------------------------------------- scenes
if want("a"):   # desk: reviewing a transaction, laptop showing the QR behind
    clear_scene()
    world((0.75, 0.72, 0.68, 1), 0.5)
    ground(linen_mat())
    laptop((1.5, 25, 0), math.radians(-4))
    dev = device("dev_a", GRAPHITE, "7_psbt_overview", (0, 0, UP), (0, 0, math.radians(12)))
    usb_cable(dev, [(-4, -9), (-11, -12), (-22, -11)])
    area("window", (-45, -20, 40), 40, 70000, (1.0, 0.96, 0.9))
    area("fill", (40, -35, 25), 25, 9000, (0.9, 0.95, 1.0))
    area("rim", (20, 40, 30), 20, 12000)
    cam_data.dof.aperture_fstop = 2.8
    shoot("10_desk.png", (2.5, -17, 10.5), (0.6, 5, 3.0), (0, 0.2, 1.7), lens=34)

if want("b"):   # flat lay on slate: device, paper SeedQR backup, cable
    clear_scene()
    world((0.5, 0.5, 0.52, 1), 0.4)
    ground(slate_mat())
    dev = device("dev_b", BONE, "4_seed_menu", (-3.2, 0.4, UP), (0, 0, math.radians(-6)))
    usb_cable(dev, [(-4.5, -9), (-1, -13), (6, -12), (14, -15)])
    seed_card((4.6, 1.6, 0), math.radians(9))
    area("key", (-25, 20, 50), 35, 60000, (1.0, 0.97, 0.92))
    area("fill", (30, -25, 35), 25, 12000, (0.9, 0.95, 1.0))
    top_down("11_flatlay.png", (0.8, -0.6), 30, lens=50)

if want("c"):   # close-up: seed loaded next to the SeedQR card it came from
    clear_scene()
    world((0.75, 0.72, 0.68, 1), 0.5)
    ground(linen_mat())
    device("dev_c", SIGNAL, "3_seed_loaded", (-3.0, 0.8, UP), (0, 0, math.radians(8)))
    seed_card((3.3, -1.0, 0), math.radians(-10))
    area("window", (-40, -10, 35), 35, 60000, (1.0, 0.96, 0.9))
    area("rim", (25, 35, 20), 20, 14000, (0.9, 0.95, 1.0))
    cam_data.dof.aperture_fstop = 3.2
    shoot("12_backup_closeup.png", (5, -23, 15), (1.2, 0, 0.8), (-1.5, 0.5, 1.7), lens=68)

if want("d"):   # copying the SeedQR onto the paper card, square by square
    clear_scene()
    world((0.75, 0.72, 0.68, 1), 0.5)
    ground(linen_mat())
    device("dev_d", BONE, "21_backup_block_1", (-3.0, 0.6, UP), (0, 0, math.radians(-7)))
    seed_card((3.6, -0.8, 0), math.radians(6))
    area("window", (-40, -10, 35), 35, 60000, (1.0, 0.96, 0.9))
    area("rim", (25, 35, 20), 20, 14000, (0.9, 0.95, 1.0))
    cam_data.dof.aperture_fstop = 4.0
    shoot("13_backup_copy.png", (2, -20, 20), (0.8, 0, 0.8), (-1.0, 0.3, 1.7), lens=60)
