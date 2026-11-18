"""Renderer-wide SilPOM anti-aliasing controls shared by every object in the open scene.

Drives the r_silpomAntiAliasing console variable. Projects can embed
RenderingControls in their own editor tools.
"""
import azlmbr.legacy.general as general
from PySide6 import QtCore, QtWidgets

AA_CHOICES=(('Project default (2× MSAA)',0),('SMAA',1),('TAA',2),('2× MSAA',4),('4× MSAA (experimental)',3))


def aa_state():
    return {'mode':int(general.get_cvar('r_silpomAntiAliasing')),
            'samples':int(general.get_cvar('r_multiSampleCount'))}


def set_aa(mode):
    if mode not in {value for _,value in AA_CHOICES}:raise ValueError('Unknown anti-aliasing selection')
    general.run_console(f'r_silpomAntiAliasing {mode}')


class RenderingControls(QtWidgets.QGroupBox):
    def __init__(self,parent=None):
        super().__init__('Rendering — whole scene',parent)
        layout=QtWidgets.QFormLayout(self)
        self.aa=QtWidgets.QComboBox(self)
        self.aa.setObjectName('SilPOMSceneAntiAliasing')
        for label,value in AA_CHOICES:self.aa.addItem(label,value)
        self.aa.setToolTip('Applies to the whole renderer, including other objects and preview scenes. SMAA and TAA use one sample. Project default restores the startup AA settings. Tower material and lighting settings are unchanged.')
        layout.addRow('Anti-aliasing',self.aa)
        self.status=QtWidgets.QLabel(self)
        self.status.setWordWrap(True)
        layout.addRow(self.status)
        self.aa.currentIndexChanged.connect(self._changed)
        self.timer=QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(500)
        self.refresh()

    def _changed(self,index):
        try:set_aa(self.aa.itemData(index))
        except (RuntimeError,ValueError,TypeError) as error:self.status.setText(str(error))

    def refresh(self):
        try:
            state=aa_state()
            self.aa.setEnabled(True)
            blocker=QtCore.QSignalBlocker(self.aa)
            self.aa.setCurrentIndex(self.aa.findData(state['mode']))
            del blocker
            self.status.setText(f"{state['samples']} sample{'s' if state['samples']!=1 else ''} per pixel. Applies to every object.")
        except (RuntimeError,ValueError,TypeError):
            self.aa.setEnabled(False)
            self.status.setText('Renderer controls are unavailable.')
