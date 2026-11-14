// SPDX-License-Identifier: MIT
#include "DecalPainter.h"
#include <SilPOM/DecalPaint.h>
#include <SilPOM/DecalPaintMath.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Asset/AssetManager.h>
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/IO/ByteContainerStream.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzCore/std/parallel/mutex.h>
#include <AzCore/std/parallel/lock.h>
#include <AzFramework/Asset/AssetCatalogBus.h>
#include <AzFramework/Entity/EntityDebugDisplayBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/API/EntityCompositionRequestBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/Entity/EditorEntityAPIBus.h>
#include <AzToolsFramework/Prefab/PrefabPublicInterface.h>
#include <AzToolsFramework/Undo/UndoSystem.h>
#include <Atom/Feature/Utils/EditorRenderComponentAdapter.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <Atom/RPI.Public/ViewportContextBus.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Pass/Pass.h>
#include <Atom/RPI.Public/Pass/PassAttachment.h>
#include <Atom/RPI.Public/Pass/AttachmentReadback.h>
#include <Atom/RHI.Reflect/Format.h>
#include <Atom/RHI/FrameEventBus.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RHI/Device.h>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCursor>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabletEvent>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QElapsedTimer>
#include <cmath>

namespace SilPOM
{
class EditorPaintComponent final : public AZ::Render::EditorRenderComponentAdapter<PaintController,PaintComponent,PaintConfig>
{
public:
    using BaseClass=AZ::Render::EditorRenderComponentAdapter<PaintController,PaintComponent,PaintConfig>;
    AZ_EDITOR_COMPONENT(EditorPaintComponent,"{2CFEDC85-B0D1-44CA-95D3-E4EE76BA640D}",BaseClass);
    static void Reflect(AZ::ReflectContext* context)
    {
        BaseClass::Reflect(context);
        if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))
        {
            sc->Class<EditorPaintComponent,BaseClass>()->Version(1);
            if(auto* ec=sc->GetEditContext())
            {
                ec->Class<EditorPaintComponent>("Surface Decal Paint","Editable layered decal strokes. Open Tools > Decal Painter to paint or edit them.")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData,"")->Attribute(AZ::Edit::Attributes::Category,"Graphics")
                    ->Attribute(AZ::Edit::Attributes::AppearsInAddComponentMenu,AZ_CRC_CE("Game"));
            }
        }
    }
};
AZ::ComponentDescriptor* CreateEditorPaintDescriptor() { return EditorPaintComponent::CreateDescriptor(); }

namespace
{
constexpr const char* PaneName="Decal Painter";
class Painter;
QPointer<QWidget> g_painter;
struct DepthFrame
{
    AZStd::mutex mutex;
    AZStd::shared_ptr<AZStd::vector<AZ::u8>> pixels;
    AZ::Matrix4x4 inverse=AZ::Matrix4x4::CreateIdentity(),camera=AZ::Matrix4x4::CreateIdentity();
    AZ::Matrix4x4 pendingInverse=AZ::Matrix4x4::CreateIdentity(),pendingCamera=AZ::Matrix4x4::CreateIdentity();
    AZ::u32 width=0,height=0;
    bool inFlight=false,valid=false;
    AZStd::string message="No readback completed";
};
AZ::RPI::ViewportContextPtr Viewport()
{
    auto* manager=AZ::Interface<AZ::RPI::ViewportContextRequestsInterface>::Get();
    return manager?manager->GetDefaultViewportContext():nullptr;
}
PaintRequests* Document(AZ::EntityId id) { return PaintRequestBus::FindFirstHandler(id); }
void SyncPaintPrefab(AZ::EntityId id)
{
    if(auto* api=AZ::Interface<AzToolsFramework::Prefab::PrefabPublicInterface>::Get())
    {
        AzToolsFramework::UndoSystem::BatchCommand sync("Synchronize paint document");
        api->GenerateUndoNodesForEntityChangeAndUpdateCache(id,&sync);
    }
}
class PaintDocumentUndo final : public AzToolsFramework::UndoSystem::URSequencePoint
{
public:
    AZ_RTTI(PaintDocumentUndo,"{6EB94304-234D-4E07-80FD-DB5EA136D5C0}",AzToolsFramework::UndoSystem::URSequencePoint);
    AZ_CLASS_ALLOCATOR(PaintDocumentUndo,AZ::SystemAllocator);
    PaintDocumentUndo(AZ::EntityId id,const PaintConfig& before,const PaintConfig& after)
        :URSequencePoint("Edit paint document"),m_entity(id),m_before(Snapshot(before)),m_after(Snapshot(after))
    {
        auto encode=[](const PaintConfig& c)
        {AZStd::string json;AZ::IO::ByteContainerStream<AZStd::string> stream(&json);AZ::JsonSerializationUtils::SaveObjectToStream(&c,stream);return json;};
        m_changed=encode(m_before)!=encode(m_after);
    }
    bool Changed() const override{return m_changed;}
    void Undo() override{Apply(m_before);}
    void Redo() override{Apply(m_after);}
private:
    static PaintConfig Snapshot(const PaintConfig& c)
    {
        // Do not copy (or retain capacity for) the growing fixed paint buffer.
        PaintConfig snapshot;snapshot.m_nextId=c.m_nextId;snapshot.m_frozenThrough=c.m_frozenThrough;
        for(const auto& layer:c.m_layers)
        {
            PaintLayer live;live.m_name=layer.m_name;live.m_visible=layer.m_visible;live.m_locked=layer.m_locked;
            live.m_opacity=layer.m_opacity;live.m_id=layer.m_id;live.m_strokes=layer.m_strokes;live.m_order=layer.m_order;
            snapshot.m_layers.push_back(AZStd::move(live));
        }
        return snapshot;
    }
    void Apply(const PaintConfig& c)
    {if(auto* d=Document(m_entity)){d->RestoreUndoDocument(c);SyncPaintPrefab(m_entity);}}
    AZ::EntityId m_entity;
    PaintConfig m_before,m_after;
    bool m_changed=false;
};
void RecordPaintUndo(AZ::EntityId id,const PaintConfig& before,AzToolsFramework::UndoSystem::URSequencePoint* batch)
{
    if(auto* d=Document(id))
    {
        auto* command=aznew PaintDocumentUndo(id,before,d->GetDocument());
        if(command->Changed())command->SetParent(batch);else delete command;
        SyncPaintPrefab(id);
    }
}
AZ::Entity* Entity(AZ::EntityId id)
{
    AZ::Entity* entity=nullptr;
    AZ::ComponentApplicationBus::BroadcastResult(entity,&AZ::ComponentApplicationRequests::FindEntity,id);return entity;
}
class Painter final : public QWidget,private AZ::TickBus::Handler,private AZ::RPI::SceneNotificationBus::Handler,
    private AzFramework::ViewportDebugDisplayEventBus::Handler,private AZ::RHI::FrameEventBus::Handler
{
public:
    explicit Painter(QWidget* parent=nullptr);
    ~Painter() override;
    AZ::EntityId CreateCanvas();
    bool Pick(int x,int y,AZ::Vector3& position,AZ::Vector3& normal);
    AZStd::string Diagnostics() const;
    void SetArmed(bool armed) { m_arm->setChecked(armed); }
private:
    void OnTick(float,AZ::ScriptTimePoint) override;
    void OnEndPrepareRender() override;
    void OnFrameCompile() override;
    void DisplayViewport(const AzFramework::ViewportInfo&,AzFramework::DebugDisplayRequests&) override;
    bool eventFilter(QObject*,QEvent*) override;
    void hideEvent(QHideEvent* e) override { Finish();m_arm->setChecked(false);QWidget::hideEvent(e); }
    void RefreshCanvases();
    void RefreshTree();
    void RefreshSelection();
    void Write(const PaintConfig&);
    void Edit(const char*,const AZStd::function<void(PaintConfig&)>&);
    void ReadBrush(PaintStroke&) const;
    void LoadBrush(const PaintStroke&);
    void Begin(QPointF,float);
    void Move(QPointF,float);
    void Finish(bool cancel=false);
    void AddPixel(QPointF,float);
    void HandlePointEdit(QPointF);
    bool ToPixel(QPointF global,QPointF& pixel) const;
    PaintLayer* Layer(PaintConfig& c) const;
    PaintStroke* Stroke(PaintConfig& c) const;
    void AddLayer();
    void LayerOrder(int);
    void DeleteSelection();
    void ApplyStroke();
    AZ::EntityId m_canvas;
    int m_layer=0,m_stroke=-1;
    AZ::u64 m_revision=~AZ::u64(0);
    AZStd::shared_ptr<DepthFrame> m_depth=AZStd::make_shared<DepthFrame>();
    AZStd::shared_ptr<AZ::RPI::AttachmentReadback> m_readback;
    AZ::RHI::Ptr<AZ::RHI::Device> m_device;
    bool m_captureMatrix=false;
    AZ::u32 m_prepareCount=0;
    AZStd::string m_passName="No pass";
    AZStd::unique_ptr<AzToolsFramework::ScopedUndoBatch> m_undo;
    PaintConfig m_before,m_work;
    AZ::EntityId m_strokeCanvas;
    AZ::Transform m_strokeInverse=AZ::Transform::CreateIdentity();
    QPointF m_lastPixel;
    float m_lastPressure=1;
    PaintMath::StrokeSampler m_sampler;
    AZ::u64 m_activeStrokeId=0;
    bool m_drawing=false,m_break=true,m_refreshing=false,m_requestDepth=false,m_hasCursor=false;
    AZ::Vector3 m_cursor=AZ::Vector3::CreateZero(),m_normal=AZ::Vector3::CreateAxisZ();
    QElapsedTimer m_clock;
    qint64 m_lastCapture=-1000,m_lastPen=-1000,m_lastCanvasRefresh=-1000;
    QComboBox *m_canvases=nullptr,*m_mode=nullptr,*m_blend=nullptr,*m_operation=nullptr;
    QCheckBox* m_paintHeights[3]{};
    QCheckBox* m_eraseHeights[3]{};
    QCheckBox *m_arm=nullptr,*m_pressureSize=nullptr,*m_pressureOpacity=nullptr,*m_visible=nullptr,*m_locked=nullptr;
    QDoubleSpinBox *m_size=nullptr,*m_opacity=nullptr,*m_spacing=nullptr,*m_rotation=nullptr,*m_depthSize=nullptr,*m_minimum=nullptr,*m_layerOpacity=nullptr;
    QDoubleSpinBox* m_normalOpacity=nullptr;
    QLineEdit *m_material=nullptr,*m_name=nullptr;
    QTreeWidget* m_tree=nullptr;
    QListWidget* m_points=nullptr;
    QLabel *m_status=nullptr,*m_pen=nullptr;
    QPushButton* m_colorButton=nullptr;
    AZ::Color m_color=AZ::Color(.18f,.12f,.07f,1.f);
};

Painter::Painter(QWidget* parent):QWidget(parent)
{
    setObjectName("DecalPainter");g_painter=this;m_clock.start();
    auto* outer=new QVBoxLayout(this);
    m_status=new QLabel(this);m_status->setObjectName("DecalPaintStatus");m_status->setWordWrap(true);outer->addWidget(m_status);
    auto* compositeHint=new QLabel("Automatic Composite: only the newest 100 dabs remain editable. Older paint is fixed and cannot be undone.",this);
    compositeHint->setWordWrap(true);outer->addWidget(compositeHint);
    auto* scroll=new QScrollArea(this);scroll->setWidgetResizable(true);outer->addWidget(scroll);
    auto* contents=new QWidget(scroll);scroll->setWidget(contents);auto* layout=new QVBoxLayout(contents);
    auto* canvasRow=new QHBoxLayout();layout->addLayout(canvasRow);
    m_canvases=new QComboBox(contents);m_canvases->setObjectName("PaintCanvas");canvasRow->addWidget(m_canvases,1);
    auto* create=new QPushButton("New paint entity",contents);create->setObjectName("NewPaintEntity");canvasRow->addWidget(create);
    connect(create,&QPushButton::clicked,this,[this]{CreateCanvas();});
    connect(m_canvases,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i)
    {if(m_refreshing)return;Finish();m_canvas=AZ::EntityId(m_canvases->itemData(i).toULongLong());m_layer=0;m_stroke=-1;m_revision=~AZ::u64(0);RefreshTree();});
    m_arm=new QCheckBox("Enable painting in viewport",contents);m_arm->setObjectName("EnablePainting");layout->addWidget(m_arm);
    connect(m_arm,&QCheckBox::toggled,this,[this](bool on){if(!on)Finish();m_requestDepth=on;});
    m_mode=new QComboBox(contents);m_mode->setObjectName("PaintMode");m_mode->addItems({"Stamp (one per click)","Paint (continuous stroke)","Edit point (drag / click to place)"});layout->addWidget(m_mode);
    connect(m_mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{Finish();});
    m_operation=new QComboBox(contents);m_operation->setObjectName("BrushOperation");m_operation->addItems({"Paint","Erase (selected layer)"});layout->addWidget(m_operation);
    m_operation->setToolTip("Erase earlier color and normal paint in the selected layer, including Composite. Brush opacity and pressure control erase strength. Newer paint is unaffected.");
    connect(m_operation,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index)
    {if(!m_refreshing)Finish();if(m_blend)m_blend->setEnabled(index==0);if(m_normalOpacity)m_normalOpacity->setEnabled(index==0);if(m_colorButton)m_colorButton->setEnabled(index==0);});
    auto* heights=new QGroupBox("Heightmap ranges",contents);layout->addWidget(heights);auto* heightForm=new QFormLayout(heights);
    for(int operation=0;operation<2;++operation)
    {
        auto* row=new QHBoxLayout();heightForm->addRow(operation?"Erase":"Paint",row);
        for(int band=0;band<3;++band)
        {
            const char* names[]={"Low","Medium","High"};auto* box=new QCheckBox(names[band],contents);
            box->setObjectName(QString("%1Height%2").arg(operation?"Erase":"Paint").arg(names[band]));box->setChecked(true);row->addWidget(box);
            (operation?m_eraseHeights:m_paintHeights)[band]=box;
            connect(box,&QCheckBox::toggled,this,[this]{if(!m_refreshing)Finish();});
        }
    }
    heights->setToolTip("Normalized heightmap: Low 0–1/3, Medium 1/3–2/3, High 2/3–1. Combine any bands. All three also includes ordinary surfaces without heightmaps. None affects nothing. Each stroke stores its own selection.");
    auto* hint=new QLabel("Left mouse / pen: paint. Alt or right mouse: navigate. Esc: cancel stroke. Ctrl+Z / Ctrl+Y: undo / redo. Save the level to keep layers and strokes.",contents);
    hint->setWordWrap(true);layout->addWidget(hint);
    auto* brushGroup=new QGroupBox("Brush / selected stroke",contents);layout->addWidget(brushGroup);auto* form=new QFormLayout(brushGroup);
    m_material=new QLineEdit("materials/decalpaint/soft_round.azmaterial",contents);m_material->setObjectName("DecalMaterial");
    auto* materialRow=new QHBoxLayout();materialRow->addWidget(m_material);
    auto* browse=new QPushButton("…",contents);materialRow->addWidget(browse);form->addRow("Material",materialRow);
    connect(browse,&QPushButton::clicked,this,[this]{auto path=QFileDialog::getOpenFileName(this,"Decal material",{},"Materials (*.material *.azmaterial)");if(path.isEmpty())return;
        auto i=path.indexOf("/Assets/",0,Qt::CaseInsensitive);if(i>=0)path=path.mid(i+8);if(path.endsWith(".material"))path.chop(9),path+=".azmaterial";m_material->setText(path);});
    auto* presetRow=new QHBoxLayout();form->addRow("Round presets",presetRow);
    for(const auto& pair:{qMakePair(QString("Soft"),QString("soft_round")),qMakePair(QString("Hard"),QString("hard_round"))})
    {auto* b=new QPushButton(pair.first,contents);presetRow->addWidget(b);connect(b,&QPushButton::clicked,this,[this,path=pair.second]{m_material->setText("materials/decalpaint/"+path+".azmaterial");});}
    auto spin=[&](const char* name,const char* title,double low,double high,double value,double step)
    {auto* s=new QDoubleSpinBox(contents);s->setObjectName(name);s->setRange(low,high);s->setDecimals(3);s->setSingleStep(step);s->setValue(value);form->addRow(title,s);return s;};
    m_size=spin("BrushDiameter","Diameter (m)",.005,100,.3,.05);
    m_opacity=spin("BrushOpacity","Dab opacity",0,1,.6,.05);
    m_normalOpacity=spin("BrushNormalOpacity","Normal map opacity",0,1,0,.05);
    m_spacing=spin("BrushSpacing","Spacing (diameters; larger = cheaper)",.05,2,.25,.05);
    m_spacing->setToolTip("Spacing controls new dabs. Existing live dab positions stay fixed; use Edit point to move them.");
    m_rotation=spin("BrushRotation","Rotation (degrees)",-360,360,0,5);
    m_depthSize=spin("ProjectionDepth","Projection depth (m)",.002,10,.12,.01);
    m_minimum=spin("MinimumPressureSize","Minimum pressure size",.01,1,.15,.05);
    m_pressureSize=new QCheckBox("Pressure controls size",contents);m_pressureSize->setObjectName("PressureSize");m_pressureSize->setChecked(true);form->addRow(m_pressureSize);
    m_pressureOpacity=new QCheckBox("Pressure controls opacity",contents);m_pressureOpacity->setObjectName("PressureOpacity");form->addRow(m_pressureOpacity);
    m_blend=new QComboBox(contents);m_blend->setObjectName("DecalBlend");m_blend->addItems({"Alpha","Multiply","Add","Screen","Overlay"});form->addRow("Blend",m_blend);
    m_colorButton=new QPushButton("Paint color",contents);m_colorButton->setObjectName("PaintColor");form->addRow(m_colorButton);
    connect(m_colorButton,&QPushButton::clicked,this,[this]{const auto c=QColorDialog::getColor(QColor::fromRgbF(m_color.GetR(),m_color.GetG(),m_color.GetB()),this);if(c.isValid())m_color=AZ::Color(float(c.redF()),float(c.greenF()),float(c.blueF()),1.f);});
    auto* apply=new QPushButton("Apply brush settings to selected stroke",contents);apply->setObjectName("ApplyStroke");form->addRow(apply);connect(apply,&QPushButton::clicked,this,[this]{ApplyStroke();});
    auto* layersGroup=new QGroupBox("Layers and strokes (bottom to top)",contents);layout->addWidget(layersGroup);auto* layers=new QVBoxLayout(layersGroup);
    auto* buttons=new QHBoxLayout();layers->addLayout(buttons);
    auto button=[&](const char* text,const char* name,auto callback){auto* b=new QPushButton(text,contents);b->setObjectName(name);buttons->addWidget(b);connect(b,&QPushButton::clicked,this,callback);};
    button("+ Layer","AddLayer",[this]{AddLayer();});button("Lower","LowerLayer",[this]{LayerOrder(-1);});button("Raise","RaiseLayer",[this]{LayerOrder(1);});button("Delete","DeletePaintSelection",[this]{DeleteSelection();});
    m_tree=new QTreeWidget(contents);m_tree->setObjectName("PaintLayers");m_tree->setHeaderHidden(true);m_tree->setMinimumHeight(150);layers->addWidget(m_tree);
    connect(m_tree,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item){if(m_refreshing||!item)return;const int layer=item->data(0,Qt::UserRole).toInt(),stroke=item->data(0,Qt::UserRole+1).toInt();Finish();m_layer=layer;m_stroke=stroke;RefreshSelection();});
    auto* layerForm=new QFormLayout();layers->addLayout(layerForm);
    m_name=new QLineEdit(contents);m_name->setObjectName("PaintItemName");layerForm->addRow("Name",m_name);
    connect(m_name,&QLineEdit::editingFinished,this,[this]{if(m_refreshing)return;Edit("Rename paint item",[&](PaintConfig& c){if(auto* s=Stroke(c))s->m_name=m_name->text().toUtf8().constData();else if(auto* l=Layer(c))l->m_name=m_name->text().toUtf8().constData();});});
    m_visible=new QCheckBox("Visible",contents);m_visible->setObjectName("PaintItemVisible");layerForm->addRow(m_visible);
    connect(m_visible,&QCheckBox::clicked,this,[this](bool v){Edit("Paint visibility",[&](PaintConfig& c){if(auto* s=Stroke(c))s->m_visible=v;else if(auto* l=Layer(c))l->m_visible=v;});});
    m_locked=new QCheckBox("Lock layer",contents);m_locked->setObjectName("PaintLayerLocked");layerForm->addRow(m_locked);
    connect(m_locked,&QCheckBox::clicked,this,[this](bool v){Edit("Lock paint layer",[&](PaintConfig& c){if(auto* l=Layer(c))l->m_locked=v;});});
    m_layerOpacity=new QDoubleSpinBox(contents);m_layerOpacity->setObjectName("LayerOpacity");m_layerOpacity->setRange(0,1);m_layerOpacity->setSingleStep(.05);layerForm->addRow("Layer opacity",m_layerOpacity);
    connect(m_layerOpacity,&QDoubleSpinBox::editingFinished,this,[this]{if(m_refreshing)return;Edit("Paint layer opacity",[&](PaintConfig& c){if(auto* l=Layer(c))l->m_opacity=float(m_layerOpacity->value());});});
    m_points=new QListWidget(contents);m_points->setObjectName("StrokePoints");m_points->setMaximumHeight(100);layers->addWidget(new QLabel("Select a path point, then place or drag it in Edit point mode.",contents));layers->addWidget(m_points);
    auto* deletePoint=new QPushButton("Delete selected point",contents);deletePoint->setObjectName("DeleteStrokePoint");layers->addWidget(deletePoint);
    connect(deletePoint,&QPushButton::clicked,this,[this]{const int point=m_points->currentRow();Edit("Delete paint point",[&](PaintConfig& c){auto* l=Layer(c);if(auto* s=Stroke(c);l&&!l->m_locked&&s&&point>=0&&size_t(point)<s->m_points.size())
        {const bool start=s->m_points[point].m_start;s->m_points.erase(s->m_points.begin()+point);if(start&&size_t(point)<s->m_points.size())s->m_points[point].m_start=true;}});});
    m_pen=new QLabel("Input: mouse. Pen pressure appears here when tablet events arrive.",contents);m_pen->setObjectName("TabletStatus");m_pen->setWordWrap(true);layout->addWidget(m_pen);
    layout->addStretch();
    qApp->installEventFilter(this);AZ::TickBus::Handler::BusConnect();
    m_device=AZ::RHI::RHISystemInterface::Get()->GetDevice();AZ::RHI::FrameEventBus::Handler::BusConnect(m_device.get());RefreshCanvases();
}
Painter::~Painter()
{
    Finish();qApp->removeEventFilter(this);AZ::TickBus::Handler::BusDisconnect();
    AZ::RHI::FrameEventBus::Handler::BusDisconnect();m_device=nullptr;
    AZ::RPI::SceneNotificationBus::Handler::BusDisconnect();AzFramework::ViewportDebugDisplayEventBus::Handler::BusDisconnect();
}
void Painter::RefreshCanvases()
{
    if(m_drawing)return;
    AZStd::vector<AZ::EntityId> ids;
    PaintRequestBus::EnumerateHandlers([&](PaintRequests* d){ids.push_back(d->GetPaintEntity());return true;});
    m_refreshing=true;m_canvases->clear();
    int selected=-1;
    for(auto id:ids) if(auto* e=Entity(id))
    {m_canvases->addItem(QString::fromUtf8(e->GetName().c_str()),QVariant::fromValue<qulonglong>(AZ::u64(id)));if(id==m_canvas)selected=m_canvases->count()-1;}
    if(selected<0&&m_canvases->count())selected=0;
    m_canvases->setCurrentIndex(selected);
    const auto id=AZ::EntityId(m_canvases->currentData().toULongLong());
    if(id!=m_canvas){m_canvas=id;m_revision=~AZ::u64(0);m_layer=0;m_stroke=-1;}
    m_refreshing=false;
}
AZ::EntityId Painter::CreateCanvas()
{
    Finish();AzToolsFramework::ScopedUndoBatch undo("Create decal paint entity");
    AZ::EntityId id;
    // The public creation path assigns a prefab alias and stable entity ID.
    // Raw EditorEntityContext creation can change IDs on the first undo reload.
    AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(id,&AzToolsFramework::ToolsApplicationRequests::CreateNewEntity,AZ::EntityId{});
    if(Entity(id))
    {
        AzToolsFramework::EditorEntityAPIBus::Event(id,&AzToolsFramework::EditorEntityAPIRequests::SetName,AZStd::string("Decal Paint"));
        AzToolsFramework::EntityCompositionRequests::AddComponentsOutcome added;
        AzToolsFramework::EntityCompositionRequestBus::BroadcastResult(added,
            &AzToolsFramework::EntityCompositionRequests::AddComponentsToEntities,
            AzToolsFramework::EntityIdList{id},AZ::ComponentTypeList{azrtti_typeid<EditorPaintComponent>()});
        if(!added.IsSuccess())return AZ::EntityId{};
        if(auto* d=Document(id)) {PaintConfig initial;initial.m_layers.emplace_back();d->SetDocument(initial);}
        AzToolsFramework::ScopedUndoBatch::MarkEntityDirty(id);m_canvas=id;m_layer=0;m_stroke=-1;m_revision=~AZ::u64(0);
        AzToolsFramework::ToolsApplicationRequestBus::Broadcast(&AzToolsFramework::ToolsApplicationRequests::SetSelectedEntities,AzToolsFramework::EntityIdList{id});
    }
    RefreshCanvases();RefreshTree();return id;
}
PaintLayer* Painter::Layer(PaintConfig& c) const {return m_layer>=0&&size_t(m_layer)<c.m_layers.size()?&c.m_layers[m_layer]:nullptr;}
PaintStroke* Painter::Stroke(PaintConfig& c) const {auto* l=Layer(c);return l&&m_stroke>=0&&size_t(m_stroke)<l->m_strokes.size()?&l->m_strokes[m_stroke]:nullptr;}
void Painter::Write(const PaintConfig& c)
{
    if(auto* d=Document(m_canvas))
    {
        d->SetDocument(c);
        if(m_drawing)
        {
            m_work=d->GetDocument();m_stroke=-1;
            for(size_t i=0;i<m_work.m_layers.size();++i)for(size_t j=0;j<m_work.m_layers[i].m_strokes.size();++j)
                if(m_work.m_layers[i].m_strokes[j].m_id==m_activeStrokeId){m_layer=int(i);m_stroke=int(j);}
        }
    }
}
void Painter::Edit(const char* name,const AZStd::function<void(PaintConfig&)>& change)
{
    Finish();if(auto* d=Document(m_canvas)) {AzToolsFramework::ScopedUndoBatch undo(name);auto before=d->GetDocument();auto c=before;change(c);Write(c);RecordPaintUndo(m_canvas,before,undo.GetUndoBatch());}RefreshTree();
}
void Painter::RefreshTree()
{
    if(m_drawing)return;
    m_refreshing=true;m_tree->clear();
    if(auto* d=Document(m_canvas))
    {
        const auto& c=d->GetDocument();m_revision=d->GetRevision();
        for(size_t i=0;i<c.m_layers.size();++i)
        {
            const auto& l=c.m_layers[i];auto* item=new QTreeWidgetItem(m_tree,{QString::fromUtf8(l.m_name.c_str())+(l.m_locked?" [locked]":"")+(!l.m_visible?" [hidden]":"")});
            item->setData(0,Qt::UserRole,int(i));item->setData(0,Qt::UserRole+1,-1);item->setExpanded(true);
            if(int(i)==m_layer&&m_stroke<0)m_tree->setCurrentItem(item);
            if(!l.m_composite.empty())
            {auto* fixed=new QTreeWidgetItem(item,{QString("Composite (%1 fixed dabs)").arg(l.m_composite.size())});fixed->setFlags(Qt::ItemIsEnabled);}
            for(size_t j=0;j<l.m_strokes.size();++j)
            {auto* s=new QTreeWidgetItem(item,{QString::fromUtf8(l.m_strokes[j].m_name.c_str())+(!l.m_strokes[j].m_visible?" [hidden]":"")});s->setData(0,Qt::UserRole,int(i));s->setData(0,Qt::UserRole+1,int(j));if(int(i)==m_layer&&int(j)==m_stroke)m_tree->setCurrentItem(s);}
        }
    }
    m_refreshing=false;RefreshSelection();
}
void Painter::RefreshSelection()
{
    m_refreshing=true;const int selectedPoint=m_points->currentRow();m_points->clear();
    if(auto* d=Document(m_canvas))
    {
        auto c=d->GetDocument();auto* l=Layer(c);auto* s=Stroke(c);
        if(l)
        {
            m_name->setText(QString::fromUtf8(s?s->m_name.c_str():l->m_name.c_str()));m_visible->setChecked(s?s->m_visible:l->m_visible);
            m_locked->setChecked(l->m_locked);m_layerOpacity->setValue(l->m_opacity);
            if(s){LoadBrush(*s);for(size_t i=0;i<s->m_points.size();++i)m_points->addItem(QString("%1  (%2, %3, %4)").arg(i).arg(s->m_points[i].m_position.GetX(),0,'f',2).arg(s->m_points[i].m_position.GetY(),0,'f',2).arg(s->m_points[i].m_position.GetZ(),0,'f',2));
                m_points->setCurrentRow(std::clamp(selectedPoint,0,m_points->count()-1));}
        }
    }
    m_refreshing=false;
}
void Painter::ReadBrush(PaintStroke& s) const
{
    AZ::Data::AssetId material;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(material,&AZ::Data::AssetCatalogRequests::GetAssetIdByPath,m_material->text().toUtf8().constData(),azrtti_typeid<AZ::RPI::MaterialAsset>(),false);
    s.m_material=material.IsValid()?AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::MaterialAsset>(material,AZ::Data::AssetLoadBehavior::QueueLoad):AZ::Data::Asset<AZ::RPI::MaterialAsset>{};
    s.m_diameter=float(m_size->value());s.m_opacity=float(m_opacity->value());s.m_spacing=float(m_spacing->value());s.m_rotation=float(m_rotation->value());s.m_depth=float(m_depthSize->value());
    s.m_normalOpacity=float(m_normalOpacity->value());
    s.m_minimumSize=float(m_minimum->value());s.m_pressureSize=m_pressureSize->isChecked();s.m_pressureOpacity=m_pressureOpacity->isChecked();s.m_color=m_color;s.m_blendMode=AZ::u32(m_blend->currentIndex());
    s.m_erase=m_operation->currentIndex()==1;s.m_heightMask=0;
    for(int i=0;i<3;++i)if((s.m_erase?m_eraseHeights:m_paintHeights)[i]->isChecked())s.m_heightMask|=1u<<i;
}
void Painter::LoadBrush(const PaintStroke& s)
{
    AZStd::string path;AZ::Data::AssetCatalogRequestBus::BroadcastResult(path,&AZ::Data::AssetCatalogRequests::GetAssetPathById,s.m_material.GetId());m_material->setText(QString::fromUtf8(path.c_str()));
    m_size->setValue(s.m_diameter);m_opacity->setValue(s.m_opacity);m_spacing->setValue(s.m_spacing);m_rotation->setValue(s.m_rotation);m_depthSize->setValue(s.m_depth);m_minimum->setValue(s.m_minimumSize);
    m_normalOpacity->setValue(s.m_normalOpacity);
    m_pressureSize->setChecked(s.m_pressureSize);m_pressureOpacity->setChecked(s.m_pressureOpacity);m_color=s.m_color;m_blend->setCurrentIndex(int(s.m_blendMode));
    m_operation->setCurrentIndex(s.m_erase?1:0);
    for(int i=0;i<3;++i)(s.m_erase?m_eraseHeights:m_paintHeights)[i]->setChecked((s.m_heightMask&(1u<<i))!=0);
}
void Painter::ApplyStroke() {Edit("Edit decal stroke",[&](PaintConfig& c){if(auto* l=Layer(c);l&&!l->m_locked)if(auto* s=Stroke(c))ReadBrush(*s);});}
void Painter::AddLayer() {Edit("Add paint layer",[&](PaintConfig& c){if(c.m_layers.size()<PaintConfig::MaxLayers){PaintLayer l;l.m_name=AZStd::string::format("Layer %zu",c.m_layers.size()+1);c.m_layers.push_back(l);m_layer=int(c.m_layers.size())-1;m_stroke=-1;}});}
void Painter::LayerOrder(int direction)
{
    Edit("Reorder paint item",[&](PaintConfig& c)
    {
        if(auto* l=Layer(c);l&&!l->m_locked&&Stroke(c))
        {
            const int next=m_stroke+direction;
            if(next<0||size_t(next)>=l->m_strokes.size())return;
            auto detachLiveTail=[&](int index)
            {
                auto& s=l->m_strokes[index];const auto old=s.m_id;
                if(AZStd::any_of(l->m_composite.begin(),l->m_composite.end(),[&](const PaintDab& d){return d.m_strokeId==old;}))
                {
                    s.m_id=c.m_nextId++;
                    const auto slot=AZStd::find(l->m_order.begin(),l->m_order.end(),old);
                    l->m_order.insert(slot==l->m_order.end()?slot:slot+1,s.m_id);
                }
            };
            // Reordering an editable tail must not move its fixed prefix.
            detachLiveTail(m_stroke);detachLiveTail(next);
            AZStd::swap(l->m_strokes[m_stroke],l->m_strokes[next]);m_stroke=next;
        }
        else
        {
            const int next=m_layer+direction;
            if(m_layer>=0&&next>=0&&size_t(next)<c.m_layers.size())
            {AZStd::swap(c.m_layers[m_layer],c.m_layers[next]);m_layer=next;}
        }
    });
}
void Painter::DeleteSelection() {Edit("Delete paint selection",[&](PaintConfig& c){if(auto* l=Layer(c);l&&!l->m_locked){if(Stroke(c)){l->m_strokes.erase(l->m_strokes.begin()+m_stroke);m_stroke=-1;}else{c.m_layers.erase(c.m_layers.begin()+m_layer);m_layer=AZStd::max(0,m_layer-1);}}});}

void Painter::OnTick(float,AZ::ScriptTimePoint)
{
    if(!isVisible())return;
    if(m_clock.elapsed()-m_lastCanvasRefresh>1000){m_lastCanvasRefresh=m_clock.elapsed();RefreshCanvases();}
    auto* doc=Document(m_canvas);
    if(!doc){if(m_drawing)Finish(true);m_status->setText("Create a paint entity to begin.");return;}
    if(!m_drawing&&doc->GetRevision()!=m_revision)RefreshTree();
    const auto viewport=Viewport();
    if(viewport&&viewport->GetRenderScene()&&(!AZ::RPI::SceneNotificationBus::Handler::BusIsConnectedId(viewport->GetRenderScene()->GetId())))
    {
        AZ::RPI::SceneNotificationBus::Handler::BusDisconnect();AZ::RPI::SceneNotificationBus::Handler::BusConnect(viewport->GetRenderScene()->GetId());
        AzFramework::EntityContextId context;
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(context,&AzToolsFramework::EditorEntityContextRequests::GetEditorEntityContextId);
        AzFramework::ViewportDebugDisplayEventBus::Handler::BusDisconnect();AzFramework::ViewportDebugDisplayEventBus::Handler::BusConnect(context);
    }
    if(m_arm->isChecked()&&m_clock.elapsed()-m_lastCapture>150)m_requestDepth=true;
    if(m_arm->isChecked()&&viewport)
    {QPointF pixel;if(ToPixel(QCursor::pos(),pixel))m_hasCursor=Pick(int(pixel.x()),int(pixel.y()),m_cursor,m_normal);else m_hasCursor=false;}
    else m_hasCursor=false;
    m_status->setText(QString::fromUtf8(doc->GetPaintStatus().c_str())+(m_arm->isChecked()?(m_hasCursor?"\nRendered surface ready.":"\nWaiting for visible surface / depth frame."):"\nPainting disabled."));
}
void Painter::OnEndPrepareRender()
{
    ++m_prepareCount;
    if(!m_requestDepth||!isVisible()||!m_arm->isChecked())return;
    // OnStartFrame rebuilds pass attachments after OnBeginPrepareRender.
    // Queue readback only after that rebuild, otherwise the request is discarded.
    const auto viewport=Viewport();if(!viewport||!viewport->GetDefaultView())return;
    const auto pipeline=viewport->GetCurrentPipeline();if(!pipeline)return;
    // The parent is always scheduled, including when a SILPOM raster child has
    // no draw items. Its output readback runs after every child (hit/fill/backup
    // and MSAA resolve), so it sees final depth without waiting on a skipped pass.
    auto pass=pipeline->FindFirstPass(AZ::Name("DepthPrePass"));AZ::Name slot("Depth");
    if(!pass){pass=pipeline->FindFirstPass(AZ::Name("DepthPass"));slot=AZ::Name("Output");}
    if(!pass||!pass->FindAttachmentBinding(slot)||!pass->FindAttachmentBinding(slot)->GetAttachment())return;
    m_passName=pass->GetName().GetCStr();
    {
        AZStd::lock_guard<AZStd::mutex> lock(m_depth->mutex);if(m_depth->inFlight)return;
        m_depth->inFlight=true;m_depth->pendingCamera=viewport->GetDefaultView()->GetWorldToClipMatrix();
        m_depth->pendingInverse=viewport->GetDefaultView()->GetClipToWorldMatrixWithOffset();
    }
    if(!m_readback)
    {
        m_readback=AZStd::make_shared<AZ::RPI::AttachmentReadback>(AZ::RHI::ScopeId("DecalPainterDepth"));
        m_readback->SetCallback([state=m_depth](const AZ::RPI::AttachmentReadback::ReadbackResult& result)
        {
            AZStd::lock_guard<AZStd::mutex> lock(state->mutex);state->inFlight=false;
            const auto size=result.m_imageDescriptor.m_size;
            state->valid=result.m_state==AZ::RPI::AttachmentReadback::ReadbackState::Success&&result.m_dataBuffer
                &&AZ::RHI::GetFormatSize(result.m_imageDescriptor.m_format)==4&&result.m_dataBuffer->size()==size_t(size.m_width)*size.m_height*4;
            if(state->valid){state->pixels=result.m_dataBuffer;state->width=size.m_width;state->height=size.m_height;state->camera=state->pendingCamera;state->inverse=state->pendingInverse;}
            state->message=AZStd::string::format("state=%u format=%u size=%ux%u bytes=%zu",AZ::u32(result.m_state),AZ::u32(result.m_imageDescriptor.m_format),size.m_width,size.m_height,result.m_dataBuffer?result.m_dataBuffer->size():0);
        });
    }
    m_captureMatrix=pass->ReadbackAttachment(m_readback,0,slot);
    if(!m_captureMatrix) {AZStd::lock_guard<AZStd::mutex> lock(m_depth->mutex);m_depth->inFlight=false;}
    m_lastCapture=m_clock.elapsed();m_requestDepth=false;
}
void Painter::OnFrameCompile()
{
    if(!m_captureMatrix)return;
    // TAA sets its jitter during Pass::FrameBegin and View::UpdateSrg runs
    // afterwards. Capture here, so unprojection matches the actual depth draw.
    if(const auto viewport=Viewport();viewport&&viewport->GetDefaultView())
    {AZStd::lock_guard<AZStd::mutex> lock(m_depth->mutex);m_depth->pendingInverse=viewport->GetDefaultView()->GetClipToWorldMatrixWithOffset();m_depth->pendingCamera=viewport->GetDefaultView()->GetWorldToClipMatrix();}
    m_captureMatrix=false;
}
bool Painter::Pick(int x,int y,AZ::Vector3& position,AZ::Vector3& normal)
{
    const auto viewport=Viewport();if(!viewport||!viewport->GetDefaultView())return false;
    AZStd::lock_guard<AZStd::mutex> lock(m_depth->mutex);
    if(!m_depth->valid||!m_depth->pixels||!m_depth->camera.IsClose(viewport->GetDefaultView()->GetWorldToClipMatrix(),1.e-5f))return false;
    const auto size=viewport->GetViewportSize();
    if(size.m_width!=m_depth->width||size.m_height!=m_depth->height||x<0||y<0||x>=int(m_depth->width)||y>=int(m_depth->height))return false;
    auto unproject=[&](int px,int py,AZ::Vector3& p)
    {
        if(px<0||py<0||px>=int(m_depth->width)||py>=int(m_depth->height))return false;
        float z;memcpy(&z,m_depth->pixels->data()+(size_t(py)*m_depth->width+px)*4,4);
        if(!std::isfinite(z)||z<=0||z>1)return false; // reversed-Z clear is zero
        const auto h=m_depth->inverse*AZ::Vector4(2.f*(float(px)+.5f)/m_depth->width-1.f,1.f-2.f*(float(py)+.5f)/m_depth->height,z,1.f);
        if(std::abs(h.GetW())<1.e-8f)return false;p=h.GetAsVector3()/h.GetW();return p.IsFinite();
    };
    if(!unproject(x,y,position))return false;
    AZ::Vector3 left,right,up,down,dx=AZ::Vector3::CreateZero(),dy=AZ::Vector3::CreateZero();
    const bool l=unproject(x-1,y,left),r=unproject(x+1,y,right),u=unproject(x,y-1,up),d=unproject(x,y+1,down);
    if(r&&(!l||(right-position).GetLengthSq()<(position-left).GetLengthSq()))dx=right-position;else if(l)dx=position-left;
    if(d&&(!u||(down-position).GetLengthSq()<(position-up).GetLengthSq()))dy=down-position;else if(u)dy=position-up;
    normal=dx.Cross(dy).GetNormalizedSafe();
    const auto towardCamera=viewport->GetCameraTransform().GetTranslation()-position;
    if(normal.GetLengthSq()<.5f)normal=towardCamera.GetNormalizedSafe();
    if(normal.Dot(towardCamera)<0)normal=-normal;
    return normal.IsFinite();
}
AZStd::string Painter::Diagnostics() const
{
    AZStd::lock_guard<AZStd::mutex> lock(m_depth->mutex);
    const auto viewport=Viewport();const auto size=viewport?viewport->GetViewportSize():AzFramework::WindowSize{};
    const bool matches=viewport&&viewport->GetDefaultView()&&m_depth->camera.IsClose(viewport->GetDefaultView()->GetWorldToClipMatrix(),1.e-5f);
    float z=0;if(m_depth->pixels&&!m_depth->pixels->empty())memcpy(&z,m_depth->pixels->data()+(size_t(m_depth->height/2)*m_depth->width+m_depth->width/2)*4,4);
    return AZStd::string::format("visible=%d armed=%d prepare=%u pass=%s inFlight=%d valid=%d cameraMatch=%d viewport=%ux%u centerDepth=%g %s",int(isVisible()),int(m_arm->isChecked()),m_prepareCount,m_passName.c_str(),int(m_depth->inFlight),int(m_depth->valid),int(matches),size.m_width,size.m_height,z,m_depth->message.c_str());
}
bool Painter::ToPixel(QPointF global,QPointF& pixel) const
{
    const auto viewport=Viewport();if(!viewport)return false;
    auto* widget=QWidget::find(reinterpret_cast<WId>(viewport->GetWindowHandle()));if(!widget)return false;
    const QPointF local=widget->mapFromGlobal(global);
    if(!widget->rect().contains(local.toPoint()))return false;
    pixel=local*viewport->GetDpiScalingFactor();return true;
}
void Painter::Begin(QPointF pixel,float pressure)
{
    if(m_drawing)return;auto* d=Document(m_canvas);if(!d)return;
    m_work=d->GetDocument();auto* layer=Layer(m_work);if(!layer||layer->m_locked||!layer->m_visible)return;
    AZ::Vector3 p,n;if(!Pick(int(pixel.x()),int(pixel.y()),p,n))return;
    PaintStroke s;ReadBrush(s);if(!s.m_material.GetId().IsValid()){m_status->setText("Choose a processed material with a base-color texture.");return;}
    m_before=m_work;m_strokeCanvas=m_canvas;
    AZ::Transform world=AZ::Transform::CreateIdentity();AZ::TransformBus::EventResult(world,m_canvas,&AZ::TransformBus::Events::GetWorldTM);m_strokeInverse=world.GetInverse();
    s.m_diameter/=AZStd::max(.001f,world.GetUniformScale());s.m_depth/=AZStd::max(.001f,world.GetUniformScale());
    s.m_id=m_work.m_nextId++;s.m_explicitDabs=true;m_activeStrokeId=s.m_id;m_sampler={};
    s.m_name=AZStd::string::format("%s %s %llu",s.m_erase?"Erase":"Paint",m_mode->currentIndex()==0?"stamp":"stroke",static_cast<unsigned long long>(s.m_id));
    m_undo=AZStd::make_unique<AzToolsFramework::ScopedUndoBatch>(s.m_erase?"Erase decal stroke":"Paint decal stroke");
    layer->m_strokes.push_back(s);m_stroke=int(layer->m_strokes.size())-1;m_drawing=true;m_break=true;m_lastPixel=pixel;m_lastPressure=pressure;AddPixel(pixel,pressure);
}
void Painter::AddPixel(QPointF pixel,float pressure)
{
    if(auto* stroke=Stroke(m_work))
    {
        if(stroke->m_points.size()>=PaintConfig::MaxPoints)return;
        AZ::Vector3 p,n;
        if(!Pick(int(pixel.x()),int(pixel.y()),p,n)){m_break=true;return;}
        PaintPoint point;point.m_position=m_strokeInverse.TransformPoint(p);point.m_normal=m_strokeInverse.TransformVector(n).GetNormalizedSafe();point.m_pressure=std::clamp(pressure,.001f,1.f);point.m_start=m_break;
        const PaintMath::Point input{{point.m_position.GetX(),point.m_position.GetY(),point.m_position.GetZ()},
            {point.m_normal.GetX(),point.m_normal.GetY(),point.m_normal.GetZ()},point.m_pressure,point.m_start};
        auto sample=input;
        if(m_sampler.started)
        {
            const float distance=(m_sampler.previous.position-input.position).Length();
            if(distance<.0001f&&!m_break)return;
            const auto a=m_sampler.previous.normal,b=input.normal;
            if(distance>AZStd::max(.05f,stroke->m_diameter*2)||a.x*b.x+a.y*b.y+a.z*b.z<.2f)sample.start=true;
        }
        m_sampler.Push(sample,[&](const PaintMath::Point& p)
        {
            PaintPoint dab;dab.m_position={p.position.x,p.position.y,p.position.z};dab.m_normal={p.normal.x,p.normal.y,p.normal.z};
            dab.m_pressure=p.pressure;dab.m_start=p.start;dab.m_sequence=m_work.m_nextId++;
            stroke->m_points.push_back(dab);
        },stroke->m_diameter,stroke->m_spacing,stroke->m_pressureSize,stroke->m_minimumSize);
        m_break=false;
    }
}
void Painter::Move(QPointF pixel,float pressure)
{
    if(!m_drawing)return;
    if(m_mode->currentIndex()==2){HandlePointEdit(pixel);return;}
    if(m_mode->currentIndex()!=1)return;
    if(m_break){m_lastPixel=pixel;m_lastPressure=pressure;AddPixel(pixel,pressure);Write(m_work);return;}
    const auto delta=pixel-m_lastPixel;const float distance=float(std::hypot(delta.x(),delta.y()));
    const int steps=std::clamp(int(std::ceil(distance)),1,2048);
    for(int i=1;i<=steps;++i){const float t=float(i)/steps;AddPixel(m_lastPixel+delta*t,m_lastPressure+(pressure-m_lastPressure)*t);}
    m_lastPixel=pixel;m_lastPressure=pressure;Write(m_work);
}
void Painter::Finish(bool cancel)
{
    if(!m_drawing)return;
    if(m_canvas==m_strokeCanvas&&Document(m_canvas))
    {
        Write(cancel?m_before:m_work);
        if(cancel)SyncPaintPrefab(m_canvas);else RecordPaintUndo(m_canvas,m_before,m_undo->GetUndoBatch());
    }
    m_drawing=false;m_undo.reset();m_activeStrokeId=0;m_strokeCanvas.SetInvalid();RefreshTree();
}
void Painter::HandlePointEdit(QPointF pixel)
{
    auto* d=Document(m_canvas);if(!d)return;
    if(!m_drawing)
    {
        m_work=d->GetDocument();auto* l=Layer(m_work);auto* s=Stroke(m_work);const int row=m_points->currentRow();
        if(!l||l->m_locked||!s||row<0||size_t(row)>=s->m_points.size())return;
        m_before=m_work;m_strokeCanvas=m_canvas;m_activeStrokeId=s->m_id;m_undo=AZStd::make_unique<AzToolsFramework::ScopedUndoBatch>("Move decal path point");m_drawing=true;
        AZ::Transform world=AZ::Transform::CreateIdentity();AZ::TransformBus::EventResult(world,m_canvas,&AZ::TransformBus::Events::GetWorldTM);m_strokeInverse=world.GetInverse();
    }
    const int row=m_points->currentRow();AZ::Vector3 p,n;
    if(auto* s=Stroke(m_work);s&&row>=0&&size_t(row)<s->m_points.size()&&Pick(int(pixel.x()),int(pixel.y()),p,n))
    {s->m_points[row].m_position=m_strokeInverse.TransformPoint(p);s->m_points[row].m_normal=m_strokeInverse.TransformVector(n).GetNormalizedSafe();Write(m_work);}
}
bool Painter::eventFilter(QObject* watched,QEvent* event)
{
    if(!isVisible()||!m_arm->isChecked())return false;
    if(event->type()==QEvent::KeyPress)
    {auto* e=static_cast<QKeyEvent*>(event);if(e->key()==Qt::Key_Escape){Finish(true);m_arm->setChecked(false);event->accept();return true;}
        if(m_drawing&&(e->key()==Qt::Key_Z||e->key()==Qt::Key_Y)&&e->modifiers().testFlag(Qt::ControlModifier))Finish();return false;}
    const bool tablet=event->type()==QEvent::TabletPress||event->type()==QEvent::TabletMove||event->type()==QEvent::TabletRelease;
    const bool mouse=event->type()==QEvent::MouseButtonPress||event->type()==QEvent::MouseMove||event->type()==QEvent::MouseButtonRelease||event->type()==QEvent::MouseButtonDblClick;
    if(!tablet&&!mouse)return false;
    QPointF global;float pressure=1;Qt::MouseButton button=Qt::NoButton;Qt::KeyboardModifiers mods;
    bool down=false,up=false;
    if(tablet)
    {
        auto* e=static_cast<QTabletEvent*>(event);global=e->globalPosition();pressure=float(e->pressure());button=e->button();mods=e->modifiers();
        down=event->type()==QEvent::TabletPress;up=event->type()==QEvent::TabletRelease;m_lastPen=m_clock.elapsed();
        m_pen->setText(QString("Pen: %1 | pressure %2 | tilt %3°, %4°").arg(e->pointingDevice()->name()).arg(pressure,0,'f',3).arg(e->xTilt()).arg(e->yTilt()));
    }
    else
    {
        auto* e=static_cast<QMouseEvent*>(event);global=e->globalPosition();button=e->button();mods=e->modifiers();
        down=event->type()==QEvent::MouseButtonPress||event->type()==QEvent::MouseButtonDblClick;up=event->type()==QEvent::MouseButtonRelease;
    }
    QPointF pixel;const bool inside=ToPixel(global,pixel);
    const auto viewport=Viewport();auto* widget=viewport?QWidget::find(reinterpret_cast<WId>(viewport->GetWindowHandle())):nullptr;
    auto* target=qobject_cast<QWidget*>(watched);
    const bool onViewport=inside&&widget&&target&&(target==widget||widget->isAncestorOf(target)||target->isAncestorOf(widget));
    if(!onViewport&&!m_drawing)return false;
    // A handled tablet event can also be followed by driver-emulated mouse input.
    if(mouse&&m_clock.elapsed()-m_lastPen<100){if(onViewport){event->accept();return true;}return false;}
    if(mods.testFlag(Qt::AltModifier)||button==Qt::RightButton||button==Qt::MiddleButton)
    {if(m_drawing)Finish();return false;}
    if(up)
    {if(m_drawing){if(inside)Move(pixel,pressure>0?pressure:m_lastPressure);Finish();event->accept();return true;}return onViewport;}
    if(down&&button==Qt::LeftButton&&onViewport)
    {
        widget->setFocus(Qt::MouseFocusReason);
        if(m_mode->currentIndex()==2)HandlePointEdit(pixel);else Begin(pixel,pressure);
        if(m_drawing)Write(m_work);event->accept();return true;
    }
    if(!down&&!up&&m_drawing)
    {if(inside)Move(pixel,pressure);else m_break=true;event->accept();return true;}
    return false;
}
void Painter::DisplayViewport(const AzFramework::ViewportInfo&,AzFramework::DebugDisplayRequests& display)
{
    if(!isVisible()||!m_arm->isChecked())return;
    if(m_hasCursor)
    {
        display.SetColor(AZ::Color(1.f,.7f,.1f,1.f));
        const auto tangent=m_normal.Cross(std::abs(m_normal.GetZ())<.9f?AZ::Vector3::CreateAxisZ():AZ::Vector3::CreateAxisY()).GetNormalizedSafe();
        const auto bitangent=m_normal.Cross(tangent);const float radius=float(m_size->value())*.5f;
        AZ::Vector3 last=m_cursor+tangent*radius;
        for(int i=1;i<=48;++i){const float a=float(i)*AZ::Constants::TwoPi/48;const auto next=m_cursor+(tangent*std::cos(a)+bitangent*std::sin(a))*radius;display.DrawLine(last,next);last=next;}
        display.DrawLine(m_cursor,m_cursor+m_normal*radius*.4f);
    }
    if(m_mode->currentIndex()==2)if(auto* d=Document(m_canvas))
    {
        auto c=d->GetDocument();if(auto* s=Stroke(c))
        {AZ::Transform world=AZ::Transform::CreateIdentity();AZ::TransformBus::EventResult(world,m_canvas,&AZ::TransformBus::Events::GetWorldTM);
            for(size_t i=0;i<s->m_points.size();++i){const auto p=world.TransformPoint(s->m_points[i].m_position);display.SetColor(int(i)==m_points->currentRow()?AZ::Color(1.f,.3f,.1f,1.f):AZ::Color(.2f,1.f,.8f,1.f));display.DrawWireSphere(p,.012f);
                if(i&&!s->m_points[i].m_start)display.DrawLine(world.TransformPoint(s->m_points[i-1].m_position),p);}}
    }
}
class PainterRequests : public AZ::EBusTraits
{
public:
    static constexpr AZ::EBusHandlerPolicy HandlerPolicy=AZ::EBusHandlerPolicy::Single;
    virtual AZ::EntityId CreatePaintEntity()=0;
    virtual AZ::Vector3 PickVisiblePixel(int,int)=0;
    virtual AZ::u64 GetViewportWindowId()=0;
    virtual AZStd::string GetPickerStatus()=0;
    virtual AZStd::string SavePaintPrefab(AZ::EntityId,const AZStd::string&)=0;
    virtual AZStd::string SetPaintDocument(AZ::EntityId,const AZStd::string&)=0;
};
using PainterBus=AZ::EBus<PainterRequests>;
class PainterAutomation final : public PainterBus::Handler
{
public:
    PainterAutomation(){BusConnect();}
    AZ::EntityId CreatePaintEntity() override {AzToolsFramework::OpenViewPane(PaneName);auto* p=static_cast<Painter*>(g_painter.data());return p?p->CreateCanvas():AZ::EntityId{};}
    AZ::Vector3 PickVisiblePixel(int x,int y) override {auto* p=static_cast<Painter*>(g_painter.data());AZ::Vector3 pos=AZ::Vector3::CreateZero(),normal;if(p)p->Pick(x,y,pos,normal);return pos;}
    AZ::u64 GetViewportWindowId() override {const auto viewport=Viewport();return viewport?reinterpret_cast<AZ::u64>(viewport->GetWindowHandle()):0;}
    AZStd::string GetPickerStatus() override {auto* p=static_cast<Painter*>(g_painter.data());return p?p->Diagnostics():AZStd::string("Painter closed");}
    AZStd::string SetPaintDocument(AZ::EntityId entity,const AZStd::string& json) override
    {
        auto* d=Document(entity);if(!d)return "Paint entity unavailable";
        AzToolsFramework::ScopedUndoBatch undo("Import paint document");
        const auto before=d->GetDocument();
        const auto error=d->SetDocumentJson(json);if(error.empty())RecordPaintUndo(entity,before,undo.GetUndoBatch());
        return error;
    }
    AZStd::string SavePaintPrefab(AZ::EntityId entity,const AZStd::string& path) override
    {
        if(!Document(entity))return "Entity has no active Surface Decal Paint component";
        auto* api=AZ::Interface<AzToolsFramework::Prefab::PrefabPublicInterface>::Get();
        if(!api)return "Prefab API unavailable";
        // Use the public path: the low-level scripting builder cannot accept
        // entities that are already owned by the level's prefab instance.
        auto created=api->CreatePrefabInMemory(AzToolsFramework::EntityIdList{entity},AZ::IO::PathView(path));
        if(!created.IsSuccess())return created.GetError();
        auto saved=api->SavePrefab(api->GetOwningInstancePrefabPath(created.GetValue()));
        return saved.IsSuccess()?AZStd::string{}:saved.GetError();
    }
};
AZStd::unique_ptr<PainterAutomation> g_automation;
}
void DecalPainterSystem::Reflect(AZ::ReflectContext* context)
{
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))sc->Class<DecalPainterSystem,AZ::Component>()->Version(1);
    if(auto* bc=azrtti_cast<AZ::BehaviorContext*>(context))bc->EBus<PainterBus>("DecalPainterBus")
        ->Attribute(AZ::Script::Attributes::Module,"silpom")->Attribute(AZ::Script::Attributes::Scope,AZ::Script::Attributes::ScopeFlags::Common)
        ->Event("CreatePaintEntity",&PainterRequests::CreatePaintEntity)->Event("PickVisiblePixel",&PainterRequests::PickVisiblePixel)
        ->Event("GetViewportWindowId",&PainterRequests::GetViewportWindowId)->Event("GetPickerStatus",&PainterRequests::GetPickerStatus)
        ->Event("SavePaintPrefab",&PainterRequests::SavePaintPrefab)->Event("SetPaintDocument",&PainterRequests::SetPaintDocument);
}
void DecalPainterSystem::Activate()
{
    EnablePaintCompositeHistory(true);
    AzToolsFramework::EditorEvents::Bus::Handler::BusConnect();g_automation=AZStd::make_unique<PainterAutomation>();
    AzToolsFramework::EditorEntityContextNotificationBus::Handler::BusConnect();
    AZ::TickBus::Handler::BusConnect();
}
void DecalPainterSystem::NotifyRegisterViews()
{
    AzToolsFramework::ViewPaneOptions options;options.paneRect=QRect(50,50,430,900);options.preferedDockingArea=Qt::RightDockWidgetArea;
    AzToolsFramework::RegisterViewPane<Painter>(PaneName,"Tools",options);
}
void DecalPainterSystem::OnPrepareForContextReset(){ResetPaintCompositeHistory();}
void DecalPainterSystem::OnTick(float,AZ::ScriptTimePoint)
{
    AzToolsFramework::UndoSystem::URSequencePoint* current=nullptr;
    AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(current,&AzToolsFramework::ToolsApplicationRequests::GetCurrentUndoBatch);
    if(current)return;
    auto* api=AZ::Interface<AzToolsFramework::Prefab::PrefabPublicInterface>::Get();if(!api)return;
    PaintRequestBus::EnumerateHandlers([&](PaintRequests* document)
    {
        if(document->ConsumeCompositeRepair())
        {
            // Repair the prefab DOM and its cache without posting a new undo
            // command or clearing history for unrelated scene edits.
            AzToolsFramework::UndoSystem::BatchCommand repair("Preserve fixed paint composite");
            api->GenerateUndoNodesForEntityChangeAndUpdateCache(document->GetPaintEntity(),&repair);
        }
        return true;
    });
}
void DecalPainterSystem::Deactivate() {g_automation.reset();AZ::TickBus::Handler::BusDisconnect();AzToolsFramework::EditorEvents::Bus::Handler::BusDisconnect();AzToolsFramework::EditorEntityContextNotificationBus::Handler::BusDisconnect();EnablePaintCompositeHistory(false);AzToolsFramework::UnregisterViewPane(PaneName);}
}
