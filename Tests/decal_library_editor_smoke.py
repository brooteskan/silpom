"""Check library rendering in the normal event loop, then exercise Editor shutdown."""
import json
from pathlib import Path
import traceback
import azlmbr.bus as bus
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom
from PySide6 import QtCore, QtWidgets

output = Path(paths.projectroot) / "user/DecalLibrarySmoke"
output.mkdir(parents=True, exist_ok=True)
result = {"passed": False, "stage": "startup"}


def save():
    (output / "result.json").write_text(json.dumps(result, indent=2))


def check():
    try:
        result["stage"] = "checking thumbnails"
        save()
        panel = next(w for w in QtWidgets.QApplication.allWidgets() if w.objectName() == "DecalPainter")
        library = panel.findChild(QtWidgets.QListWidget, "DecalLibrary")
        library.scrollToTop()
        library.grab().save(str(output / "library.png"))
        names = [library.item(i).text() for i in range(library.count())]
        assert len(names) == 4, names
        result["entries"] = names
        for i in range(library.count()):
            item = library.item(i)
            assert not item.icon().isNull()
            rect = library.visualItemRect(item)
            assert rect.height() >= 120, (item.text(), rect.height())
        for query in ("0001", "0002"):
            panel.findChild(QtWidgets.QLineEdit, "DecalLibrarySearch").setText(query)
            assert sum(not library.item(i).isHidden() for i in range(library.count())) == 1
        panel.findChild(QtWidgets.QLineEdit, "DecalLibrarySearch").clear()
        panel.findChild(QtWidgets.QPushButton, "RefreshDecalLibrary").click()
        result["passed"] = True
        result["stage"] = "complete"
    except Exception:
        result["error"] = traceback.format_exc()
    save()
    QtCore.QTimer.singleShot(500, general.exit_no_prompt)


save()
general.idle_enable(True)
assert general.open_level(str(Path(paths.projectroot) / "Levels/VueLiveLink/VueLiveLink.prefab"))
general.idle_wait_frames(60)
canvas = silpom.DecalPainterBus(bus.Broadcast, "CreatePaintEntity")
QtCore.QTimer.singleShot(2000, check)
