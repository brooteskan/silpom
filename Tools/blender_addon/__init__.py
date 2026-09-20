"""Blender UI for the shared SilPOM authoring transport exporter."""

bl_info = {
    'name': 'SilPOM Face Exporter',
    'author': 'SilPOM contributors',
    'version': (0, 2, 0),
    'blender': (5, 0, 0),
    'location': 'File > Export; 3D View > Sidebar > SilPOM',
    'description': 'Tag planar mesh faces and export FBX with SilPOM metadata',
    'warning': 'Experimental: tagged/ordinary displacement boundaries remain open',
    'category': 'Import-Export',
}

from pathlib import Path

import bmesh
import bpy
from bpy.props import BoolProperty, IntProperty, PointerProperty, StringProperty
from bpy_extras.io_utils import ExportHelper

from .export_mesh import export, output_paths, require


class SILPOM_PG_settings(bpy.types.PropertyGroup):
    region: IntProperty(name='Region', description='Positive displacement region ID', default=1, min=1)
    profile: IntProperty(name='Profile', description='Stable profile reference, not a height map assignment', default=1, min=1)


class SILPOM_OT_assign_region(bpy.types.Operator):
    bl_idname = 'silpom.assign_region'
    bl_label = 'Assign Displacement Region'
    bl_description = 'Assign the region/profile to selected faces; unassigned faces remain ordinary geometry'
    bl_options = {'REGISTER', 'UNDO'}

    ordinary: BoolProperty(default=False, options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH' and context.active_object is not None

    def execute(self, context):
        targets = []
        try:
            # Validate all edited meshes before changing any of them.
            for obj in context.objects_in_mode_unique_data:
                mesh = obj.data
                bm = bmesh.from_edit_mesh(mesh)
                bm.faces.index_update()
                faces = [face.index for face in bm.faces if face.select and not face.hide]
                if not faces:
                    continue
                require(obj.library is None and mesh.library is None, 'Make linked meshes local before tagging')
                for name in ('silpom_region_id', 'silpom_profile_id'):
                    attribute = mesh.attributes.get(name)
                    require(attribute is None or (attribute.data_type == 'INT' and attribute.domain == 'FACE'),
                            f'{obj.name}: {name} must be Integer / Face')
                targets.append((mesh, bm, faces))
            require(targets, 'Select at least one face in Edit Mode')
        except ValueError as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}

        settings = context.scene.silpom_export
        count = 0
        for mesh, bm, faces in targets:
            regions = bm.faces.layers.int.get('silpom_region_id')
            if regions is None:
                regions = bm.faces.layers.int.new('silpom_region_id')
            profiles = bm.faces.layers.int.get('silpom_profile_id')
            if profiles is None:
                profiles = bm.faces.layers.int.new('silpom_profile_id')
                # Preserve the exporter's implicit profile on previously tagged faces.
                for face in bm.faces:
                    face[profiles] = 1 if face[regions] else 0
            # Adding a custom-data layer can invalidate cached BMFace wrappers.
            bm.faces.ensure_lookup_table()
            regions = bm.faces.layers.int['silpom_region_id']
            profiles = bm.faces.layers.int['silpom_profile_id']
            for index in faces:
                face = bm.faces[index]
                face[regions] = 0 if self.ordinary else settings.region
                face[profiles] = 0 if self.ordinary else settings.profile
                count += 1
            bmesh.update_edit_mesh(mesh, loop_triangles=False, destructive=False)
        self.report({'INFO'}, f'Assigned {count} faces to ' + ('ordinary geometry' if self.ordinary else f'region {settings.region}'))
        return {'FINISHED'}


class SILPOM_OT_export(bpy.types.Operator, ExportHelper):
    bl_idname = 'export_scene.silpom'
    bl_label = 'Export SilPOM Faces'
    bl_description = 'Write an FBX, matching SilPOM sidecar, and ID-preserving authoring copy'
    filename_ext = '.fbx'

    filter_glob: StringProperty(default='*.fbx', options={'HIDDEN'})
    use_selection: BoolProperty(name='Selected Objects Only', default=True,
                               description='Export selected mesh objects; disable to export all scene meshes')
    initialize_identities: BoolProperty(name='Initialize / Repair IDs', default=True,
                                       description='Allocate missing IDs and repair duplicates while preserving valid IDs; disable for strict validation')
    overwrite: BoolProperty(name='Overwrite Export Files', default=False,
                            description='Allow replacing the FBX, sidecar and authoring copy, but never the currently open blend file')

    @classmethod
    def poll(cls, context):
        return context.mode in {'OBJECT', 'EDIT_MESH'}

    def draw(self, _context):
        layout = self.layout
        layout.prop(self, 'use_selection')
        layout.prop(self, 'initialize_identities')
        layout.prop(self, 'overwrite')
        layout.separator()
        layout.label(text='Writes FBX + .silpom.json + .authoring.blend')
        layout.label(text='Tagged faces blend normals on import', icon='INFO')

    def execute(self, context):
        edit_mode = context.mode == 'EDIT_MESH'
        try:
            require(bool(self.filepath), 'Choose an FBX filename')
            filepath = Path(bpy.path.abspath(self.filepath)).resolve()
            require(filepath.suffix.lower() == '.fbx', 'Choose a filename ending in .fbx')
            paths = output_paths(filepath.parent, filepath.stem)
            existing = [path.name for path in paths.values() if path.exists()]
            require(self.overwrite or not existing,
                    'Export files already exist: ' + ', '.join(existing) + '. Choose another name or enable Overwrite Export Files')
            candidates = context.selected_objects if self.use_selection else context.scene.objects
            objects = [obj for obj in candidates if obj.type == 'MESH']
            require(objects, 'Select at least one mesh, or disable Selected Objects Only')
            if edit_mode:
                bpy.ops.object.mode_set(mode='OBJECT')
            document = export(filepath.parent, objects, initialize=self.initialize_identities, basename=filepath.stem)
        except Exception as error:
            self.report({'ERROR'}, f'SilPOM export failed: {error}')
            return {'CANCELLED'}
        finally:
            if edit_mode and context.mode == 'OBJECT':
                bpy.ops.object.mode_set(mode='EDIT')
        self.report({'INFO'}, f'Exported {len(document["faces"])} triangles. IDs retained in this session; authoring copy: {paths["authoring"].name}')
        return {'FINISHED'}


class SILPOM_PT_authoring(bpy.types.Panel):
    bl_label = 'SilPOM Faces'
    bl_idname = 'SILPOM_PT_authoring'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'SilPOM'

    def draw(self, context):
        layout = self.layout
        layout.label(text='Experimental authoring transport', icon='INFO')
        layout.label(text='Normals blend between tagged faces')
        column = layout.column()
        column.enabled = context.mode == 'EDIT_MESH'
        column.prop(context.scene.silpom_export, 'region')
        column.prop(context.scene.silpom_export, 'profile')
        column.operator('silpom.assign_region', text='Assign to Selected Faces').ordinary = False
        column.operator('silpom.assign_region', text='Mark Selected Faces Ordinary').ordinary = True
        if context.mode != 'EDIT_MESH':
            layout.label(text='Enter Edit Mode to tag faces')
        layout.separator()
        layout.operator('export_scene.silpom', text='Export SilPOM Faces', icon='EXPORT')
        layout.label(text='Requires UVs, materials and baked modifiers')


def menu_export(self, _context):
    self.layout.operator(SILPOM_OT_export.bl_idname, text='SilPOM Faces (.fbx + .json)')


CLASSES = (SILPOM_PG_settings, SILPOM_OT_assign_region, SILPOM_OT_export, SILPOM_PT_authoring)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.silpom_export = PointerProperty(type=SILPOM_PG_settings)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    del bpy.types.Scene.silpom_export
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
