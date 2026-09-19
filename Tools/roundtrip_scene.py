"""O3DE ScriptProcessorRule: inspect the imported graph before model export.

Only attached to the dedicated fixture. Writes a report beside its source FBX;
does not change ordinary model selection or any other scene's manifest.
"""
import json
import hashlib
from pathlib import Path
import traceback
import azlmbr.scene


def inspect(args):
    scene = args[0]
    output = Path(scene.sourceFilename).with_suffix('.imported.json')
    report = {'source': scene.sourceFilename, 'meshes': [], 'error': None,
              'fbx_sha256': hashlib.sha256(Path(scene.sourceFilename).read_bytes()).hexdigest()}
    try:
        graph = scene.graph
        pending = [graph.GetRoot()]
        while pending:
            node = pending.pop()
            if graph.HasNodeSibling(node):
                pending.append(graph.GetNodeSibling(node))
            if graph.HasNodeChild(node):
                pending.append(graph.GetNodeChild(node))
            content = graph.GetNodeContent(node)
            if not content or not content.CastWithTypeName('MeshData'):
                continue
            mesh = {'path': graph.GetNodeName(node).GetPath(), 'positions': [], 'normals': [], 'faces': [], 'uv': {}, 'materials': []}
            for i in range(content.Invoke('GetVertexCount', [])):
                p = content.Invoke('GetPosition', [i])
                n = content.Invoke('GetNormal', [i])
                mesh['positions'].append([p.x, p.y, p.z])
                mesh['normals'].append([n.x, n.y, n.z])
            for i in range(content.Invoke('GetFaceCount', [])):
                mesh['faces'].append({'indices': [content.Invoke('GetVertexIndex', [i, j]) for j in range(3)],
                                      'material': content.Invoke('GetFaceMaterialId', [i])})
            child = graph.GetNodeChild(node)
            while child.IsValid():
                data = graph.GetNodeContent(child)
                if data and data.CastWithTypeName('MeshVertexUVData'):
                    mesh['uv'][graph.GetNodeName(child).GetName()] = [[data.Invoke('GetUV', [i]).x, data.Invoke('GetUV', [i]).y]
                                                                 for i in range(data.Invoke('GetCount', []))]
                elif data and data.CastWithTypeName('MaterialData'):
                    mesh['materials'].append(graph.GetNodeName(child).GetName())
                child = graph.GetNodeSibling(child)
            report['meshes'].append(mesh)
    except Exception:
        report['error'] = traceback.format_exc()
    output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('SILPOM_IMPORT_REPORT ' + str(output))
    handler.disconnect()
    # Empty return retains the existing manifest and stock model export.
    return ''


handler = azlmbr.scene.ScriptBuildingNotificationBusHandler()
handler.connect()
handler.add_callback('OnUpdateManifest', inspect)
