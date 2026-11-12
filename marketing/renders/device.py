"""Shared pieces for the product renders: materials, the real case mesh with a
firmware screen on its display, render settings and a camera helper.

Originally part of render.py; imported by render.py and scenes.py.

Product renders of the stateless signer.

Uses the real printed case (case/exports/case.stl) and the firmware's actual
screens (firmware/docs/screens/*.png, rendered by the simulator), so the
pictures show what the device really looks like.

Run from the repo root:
    blender -b -P marketing/renders/render.py
Options (env): SAMPLES=256, PCT=100, ONLY=1,3 (shot numbers)
Units: 1 Blender unit = 1 cm; the STL is in mm with the glass front at z=0.
"""
import bpy, bmesh, math, sys, os
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CASE_STL = os.path.join(ROOT, "case", "exports", "case.stl")
SCREENS = os.path.join(ROOT, "firmware", "docs", "screens")
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

ZB, ZT = -1.68, 0.09               # case bottom / rim top (cm)
GW, GH, GR = 3.71, 5.88, 0.26      # cover glass
SW, SH, SCY = 3.10, 4.12, 0.129    # active display (240x320), centre y
CAMY = 0.8                         # lens hole centre y

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


def mat(name, color, rough=0.5, metal=0.0, coat=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = color
    p.inputs["Roughness"].default_value = rough
    p.inputs["Metallic"].default_value = metal
    p.inputs["Coat Weight"].default_value = coat
    return m


def rounded_box(name, w, h, d, r, edge, material, parent, loc=(0, 0, 0)):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=(w, h, d), verts=bm.verts)
    vert_edges = [e for e in bm.edges if abs((e.verts[0].co - e.verts[1].co).normalized().z) > 0.99]
    bmesh.ops.bevel(bm, geom=vert_edges, offset=r, segments=12, affect='EDGES', profile=0.5)
    bm.to_mesh(me); bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(ob)
    for poly in me.polygons:
        poly.use_smooth = True
    if edge > 0:
        bv = ob.modifiers.new("edge", 'BEVEL')
        bv.width = edge; bv.segments = 6; bv.limit_method = 'ANGLE'
        bv.angle_limit = math.radians(40); bv.harden_normals = True
    ob.modifiers.new("wn", 'WEIGHTED_NORMAL').keep_sharp = True
    me.materials.append(material)
    ob.location = loc; ob.parent = parent
    return ob


m_glass = mat("glass", (0.004, 0.004, 0.004, 1), rough=0.03, coat=1.0)
m_dark = mat("interior", (0.01, 0.01, 0.01, 1), rough=0.8)
m_metal = mat("metal", (0.6, 0.6, 0.62, 1), rough=0.25, metal=1.0)
m_lens = mat("lens", (0.01, 0.012, 0.05, 1), rough=0.02, coat=1.0)

_screen_mats = {}


def screen_mat(name):
    """Emissive material showing a simulator screenshot, pixel for pixel."""
    if name in _screen_mats:
        return _screen_mats[name]
    img = bpy.data.images.load(os.path.join(SCREENS, name + ".png"))
    m = bpy.data.materials.new("screen_" + name)
    m.use_nodes = True
    nt = m.node_tree
    p = nt.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (0, 0, 0, 1)
    p.inputs["Roughness"].default_value = 0.04
    p.inputs["Coat Weight"].default_value = 1.0
    p.inputs["Emission Strength"].default_value = 1.7
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.interpolation = 'Closest'          # a 240x320 LCD: keep the pixels crisp
    nt.links.new(tex.outputs["Color"], p.inputs["Emission Color"])
    _screen_mats[name] = m
    return m


bpy.ops.wm.stl_import(filepath=CASE_STL)
CASE_SRC = bpy.context.selected_objects[0]
CASE_SRC.scale = (0.1, 0.1, 0.1)
bpy.ops.object.shade_auto_smooth(angle=math.radians(35))
CASE_SRC.hide_render = True; CASE_SRC.hide_viewport = True


def device(name, body_color, screen, loc, rot):
    root = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(root)
    shell = CASE_SRC.copy()
    bpy.context.collection.objects.link(shell)
    shell.hide_render = False; shell.hide_viewport = False
    shell.parent = root
    if not shell.material_slots:
        shell.data.materials.append(None)
    shell.material_slots[0].link = 'OBJECT'
    shell.material_slots[0].material = mat(name + "_pla", body_color, rough=0.62)
    rounded_box(name + "_inner", 3.6, 5.8, 1.35, 0.2, 0.0, m_dark, root, (0, 0, -0.78))
    rounded_box(name + "_glass", GW, GH, 0.06, GR, 0.015, m_glass, root, (0, 0, -0.03))
    bpy.ops.mesh.primitive_plane_add(size=1, location=(0, SCY, 0.001))
    disp = bpy.context.object
    disp.scale = (SW, SH, 1); disp.parent = root
    disp.data.materials.append(screen_mat(screen))
    # USB-C receptacle seen through the cutout
    rounded_box(name + "_usb", 0.894, 0.735, 0.326, 0.15, 0.0, m_metal, root, (0, -2.04, -0.883))
    rounded_box(name + "_usbin", 0.80, 0.02, 0.24, 0.11, 0.0, m_dark, root, (0, -2.415, -0.883))
    # camera lens in the back
    bpy.ops.mesh.primitive_cylinder_add(radius=0.3, depth=0.12, vertices=96, location=(0, CAMY, ZB + 0.12))
    barrel = bpy.context.object; barrel.parent = root; barrel.data.materials.append(m_dark)
    bpy.ops.object.shade_smooth()
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.2, segments=64, ring_count=32, location=(0, CAMY, ZB + 0.06))
    lens = bpy.context.object; lens.scale = (1, 1, 0.3); lens.parent = root
    lens.data.materials.append(m_lens); bpy.ops.object.shade_smooth()
    root.location = loc; root.rotation_euler = rot
    return root



# ---------- camera and render settings ----------
cam_data = bpy.data.cameras.new("cam")
cam_data.dof.use_dof = True; cam_data.dof.aperture_fstop = 4.0
cam = bpy.data.objects.new("cam", cam_data); bpy.context.collection.objects.link(cam)
scene.camera = cam


def shoot(fname, cam_loc, target, focus, lens=85):
    cam.location = cam_loc; cam_data.lens = lens
    cam.rotation_euler = (Vector(target) - Vector(cam_loc)).to_track_quat('-Z', 'Y').to_euler()
    cam_data.dof.focus_distance = (Vector(focus) - Vector(cam_loc)).length
    scene.render.filepath = os.path.join(OUT, fname)
    bpy.ops.render.render(write_still=True)
    print("wrote", scene.render.filepath)


scene.render.engine = 'CYCLES'
prefs = bpy.context.preferences.addons["cycles"].preferences
for backend in ("OPTIX", "CUDA"):
    try:
        prefs.compute_device_type = backend
        devs = prefs.get_devices_for_type(backend)
        if not devs: continue
        for d in devs: d.use = True
        scene.cycles.device = 'GPU'; break
    except Exception:
        continue
scene.cycles.samples = int(os.environ.get("SAMPLES", 256))
scene.cycles.use_denoising = True
scene.render.resolution_x, scene.render.resolution_y = 1600, 1200
scene.render.resolution_percentage = int(os.environ.get("PCT", 100))
scene.view_settings.view_transform = 'AgX'
scene.view_settings.look = 'AgX - Medium High Contrast'


GRAPHITE = (0.03, 0.03, 0.028, 1)
SIGNAL = (0.78, 0.30, 0.04, 1)
BONE = (0.70, 0.68, 0.63, 1)
UP = -ZB + 0.001       # face up, back on the floor
DOWN = ZT + 0.001      # face down, rim on the floor


def clear_devices():
    for o in [o for o in bpy.data.objects
              if o.name.startswith("dev") or (o.parent and o.parent.name.startswith("dev"))]:
        bpy.data.objects.remove(o, do_unlink=True)
