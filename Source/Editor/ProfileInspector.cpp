// SPDX-License-Identifier: MIT
#include "ProfileInspector.h"
#include <SilPOM/Mesh.h>
#include <SilPOM/SurfaceProfileAsset.h>
#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Utils/Utils.h>
#include <AzFramework/Asset/AssetSystemBus.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <cstring>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

namespace SilPOM
{
namespace
{
constexpr const char* ProfilePaneName = "SilPOM Profiles";

// Role each shader model was trained for (silpom_profile.py MODELS).
const char* ModelRole(int model) { return model == 1 || model == 4 ? "Wall" : model == 6 || model == 7 ? "Opening" : "Surface"; }
const char* RoleName(AZ::u32 role) { return role == 1 ? "Wall" : role == 2 ? "Opening" : "Surface"; }
QString ModelDescription(int model)
{
    switch (model)
    {
    case 0: return "0 - model 0: seed network + no-march hit (optional seed cache)";
    case 1: return "1 - welded wall";
    case 2: return "2 - welded stairs";
    case 3: return "3 - welded door";
    case 4: return "4 - open-shaft wall (Level 2, visibility, estimators)";
    case 5: return "5 - open-shaft stairs (Level 2, visibility)";
    case 6: return "6 - open-shaft trim (Level 2, visibility)";
    default: return "7 - open-shaft reveal (Level 2, visibility)";
    }
}
// Profile values are float32; 7 significant digits print them without double noise.
QString Number(double value) { return QLocale::c().toString(value, 'g', 7); }
QString NetworkList(AZ::u32 flags)
{
    QStringList names{"hit model"};
    if (flags & SurfaceProfileAsset::LevelTwoFlag) names << "Level 2";
    if (flags & SurfaceProfileAsset::VisibilityFlag) names << "visibility" << "physical visibility";
    if (flags & SurfaceProfileAsset::SeedFlag) names << "seed network";
    if (flags & SurfaceProfileAsset::CacheLayoutFlag) names << "seed cache layout";
    for (size_t i = 0; i < SurfaceProfileAsset::EstimatorCount; ++i)
        if (flags & SurfaceProfileAsset::EstimatorFlag(i)) names << SurfaceProfileAsset::EstimatorName(i);
    return names.join(", ");
}
// Absolute source file of a product path, if the Asset Processor knows it.
QString SourceOfProduct(const QString& product)
{
    if (product.isEmpty()) return {};
    AZ::Data::AssetId id;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(id, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
        product.toUtf8().constData(), AZ::Data::AssetType::CreateNull(), false);
    if (!id.IsValid()) return {};
    bool found = false;
    AZ::Data::AssetInfo info;
    AZStd::string watchFolder;
    AzToolsFramework::AssetSystemRequestBus::BroadcastResult(found, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourceUUID,
        id.m_guid, info, watchFolder);
    return found ? QDir(QString::fromUtf8(watchFolder.c_str())).filePath(QString::fromUtf8(info.m_relativePath.c_str())) : QString();
}
QString SourceOfAsset(const AZ::Data::AssetId& id)
{
    bool found = false;
    AZ::Data::AssetInfo info;
    AZStd::string watchFolder;
    AzToolsFramework::AssetSystemRequestBus::BroadcastResult(found, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourceUUID,
        id.m_guid, info, watchFolder);
    return found ? QDir(QString::fromUtf8(watchFolder.c_str())).filePath(QString::fromUtf8(info.m_relativePath.c_str())) : QString();
}
AZ::EntityId SelectedMesh()
{
    AzToolsFramework::EntityIdList selected;
    AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(selected, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
    for (const auto& id : selected)
        if (MeshRequestBus::FindFirstHandler(id)) return id;
    return AZ::EntityId();
}

// Creates a .silpomprofile and optionally assigns it to a profile ID.
class CreateProfileDialog final : public QDialog
{
public:
    CreateProfileDialog(QWidget* parent, AZ::EntityId mesh, AZ::u32 profileId) : QDialog(parent), m_mesh(mesh)
    {
        setWindowTitle("Create SilPOM surface profile");
        auto* form = new QFormLayout;
        const auto browseRow = [this](QLineEdit* edit, auto browse)
        {
            auto* row = new QHBoxLayout;
            auto* button = new QPushButton("...", this);
            row->addWidget(edit, 1);
            row->addWidget(button);
            connect(button, &QPushButton::clicked, this, browse);
            return row;
        };
        m_height = new QLineEdit(this);
        m_height->setPlaceholderText("Normalized height image in the project (PNG, TIF, EXR or DDS)");
        form->addRow("Height image", browseRow(m_height, [this]
        {
            const auto file = QFileDialog::getOpenFileName(this, "Height image", ProjectAssets(), "Height images (*.png *.tif *.tiff *.exr *.dds)");
            if (!file.isEmpty()) { m_height->setText(QDir::toNativeSeparators(file)); OnHeight(); }
        }));
        connect(m_height, &QLineEdit::editingFinished, this, [this] { OnHeight(); });
        m_width = new QSpinBox(this); m_width->setRange(1, 65536);
        m_pixels = new QSpinBox(this); m_pixels->setRange(1, 65536);
        auto* size = new QHBoxLayout; size->addWidget(m_width); size->addWidget(new QLabel("x", this)); size->addWidget(m_pixels); size->addStretch();
        form->addRow("Size (pixels)", size);
        m_scale = new QDoubleSpinBox(this); m_scale->setRange(0.0001, 10); m_scale->setDecimals(4); m_scale->setSingleStep(0.01); m_scale->setValue(0.1);
        m_scale->setSuffix(" m");
        form->addRow("Displacement", m_scale);
        m_reference = new QDoubleSpinBox(this); m_reference->setRange(0, 1); m_reference->setDecimals(4); m_reference->setSingleStep(0.05); m_reference->setValue(0.05);
        form->addRow("Reference height", m_reference);
        m_address = new QComboBox(this); m_address->addItems({"Repeat", "Clamp"});
        form->addRow("Addressing", m_address);
        m_model = new QComboBox(this);
        for (int model = 0; model <= 7; ++model) m_model->addItem(ModelDescription(model), model);
        m_model->setCurrentIndex(4);
        form->addRow("Shader model", m_model);
        m_role = new QLabel(this);
        form->addRow("Role", m_role);
        m_networks = new QLineEdit(this);
        m_networks->setPlaceholderText("Optional: folder of trainer output headers (else the reference networks)");
        form->addRow("Trained networks", browseRow(m_networks, [this]
        {
            const auto folder = QFileDialog::getExistingDirectory(this, "Trained networks folder", ProjectAssets());
            if (!folder.isEmpty()) m_networks->setText(QDir::toNativeSeparators(folder));
        }));
        m_file = new QLineEdit(this);
        form->addRow("Profile file", browseRow(m_file, [this]
        {
            const auto file = QFileDialog::getSaveFileName(this, "Profile file", m_file->text().isEmpty() ? ProjectAssets() : m_file->text(),
                "SilPOM surface profiles (*.silpomprofile)");
            if (!file.isEmpty()) m_file->setText(QDir::toNativeSeparators(file.endsWith(".silpomprofile") ? file : file + ".silpomprofile"));
        }));
        m_assign = new QComboBox(this);
        m_assign->addItem("Do not assign");
        if (m_mesh.IsValid())
        {
            m_assign->addItem("This entity (Trained profile override)");
            m_assign->addItem("The surface, for every use (.silpomprofiles)");
            m_assign->setCurrentIndex(1);
        }
        m_profileId = new QSpinBox(this); m_profileId->setRange(1, 65535); m_profileId->setValue(int(profileId));
        auto* assign = new QHBoxLayout; assign->addWidget(m_assign, 1); assign->addWidget(new QLabel("Profile ID", this)); assign->addWidget(m_profileId);
        form->addRow("Assign to", assign);
        m_note = new QLabel(this);
        m_note->setWordWrap(true);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText("Create");
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { Create(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto* layout = new QVBoxLayout(this);
        layout->addLayout(form);
        layout->addWidget(m_note);
        layout->addWidget(buttons);
        connect(m_model, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { Update(); });
        connect(m_networks, &QLineEdit::textChanged, this, [this] { Update(); });
        Update();
        resize(640, sizeHint().height());
    }
    // The profile written, and how it was assigned (0 none, 1 override, 2 surface).
    QString Profile() const { return m_written; }
    int Assignment() const { return m_assign->currentIndex(); }
    AZ::u32 ProfileId() const { return AZ::u32(m_profileId->value()); }

private:
    static QString ProjectAssets()
    {
        return QDir(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).filePath("Assets");
    }
    void OnHeight()
    {
        const QString path = m_height->text();
        QImageReader reader(path);
        if (const auto size = reader.size(); size.isValid()) { m_width->setValue(size.width()); m_pixels->setValue(size.height()); }
        if (m_file->text().isEmpty() && !path.isEmpty())
        {
            const QFileInfo image(path);
            m_file->setText(QDir::toNativeSeparators(image.dir().filePath(image.completeBaseName() + ".silpomprofile")));
        }
        Update();
    }
    void Update()
    {
        const int model = m_model->currentData().toInt();
        m_role->setText(ModelRole(model));
        const bool image = !m_height->text().isEmpty() && QImageReader(m_height->text()).size().isValid();
        m_width->setEnabled(!image);
        m_pixels->setEnabled(!image);
        QString note = model == 0
            ? "Model 0 without a height image applies to each binding's own image of this size and calibration."
            : "The profile supplies its height image, displacement, reference and addressing to every profile ID it is assigned to.";
        note += m_networks->text().isEmpty()
            ? " Its weights are written from the reference networks of this shader model (trained for the Irish Fieldstone tower "
              "atlases) when the Asset Processor builds it. Retrain with SilPOM's trainers for other images; see Docs/SurfaceProfiles.md."
            : " Its weights are written from the trained networks folder, falling back to the reference networks.";
        m_note->setText(note);
    }
    void Create()
    {
        const int model = m_model->currentData().toInt();
        const QString file = QDir::fromNativeSeparators(m_file->text());
        const QString height = QDir::fromNativeSeparators(m_height->text());
        const QString networks = QDir::fromNativeSeparators(m_networks->text());
        const auto fail = [this](const QString& message) { QMessageBox::warning(this, windowTitle(), message); };
        if (!file.endsWith(".silpomprofile")) return fail("Choose a profile file ending in .silpomprofile.");
        if (height.isEmpty() && model != 0) return fail("Choose the height image the profile is for. Only model 0 can omit it.");
        if (!height.isEmpty() && !QFileInfo::exists(height)) return fail("The height image does not exist.");
        if (!networks.isEmpty() && !QFileInfo(networks).isDir()) return fail("The trained networks folder does not exist.");
        if (QFileInfo::exists(file)
            && QMessageBox::question(this, windowTitle(), "Replace the existing profile file? Its weights file is kept.") != QMessageBox::Yes)
            return;
        const QDir folder = QFileInfo(file).dir();
        if (!height.isEmpty() && folder.relativeFilePath(height).startsWith("../../../../"))
            return fail("Keep the profile near its height image, in the same asset scan folder.");
        QStringList lines{"  \"Version\": 1"};
        if (!height.isEmpty()) lines << QString("  \"Height\": \"%1\"").arg(folder.relativeFilePath(height));
        lines << QString("  \"Width\": %1").arg(m_width->value()) << QString("  \"HeightPixels\": %1").arg(m_pixels->value())
              << QString("  \"Scale\": %1").arg(Number(m_scale->value())) << QString("  \"Reference\": %1").arg(Number(m_reference->value()))
              << QString("  \"AddressMode\": %1").arg(m_address->currentIndex())
              << QString("  \"Role\": \"%1\"").arg(ModelRole(model)) << QString("  \"ShaderModel\": %1").arg(model);
        if (!networks.isEmpty()) lines << QString("  \"Networks\": \"%1\"").arg(folder.relativeFilePath(networks));
        QFile out(file);
        if (!folder.exists() || !out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return fail("Cannot write " + file);
        out.write(("{\n" + lines.join(",\n") + "\n}\n").toUtf8());
        out.close();
        m_written = file;
        accept();
    }

    AZ::EntityId m_mesh;
    QString m_written;
    QLineEdit *m_height = nullptr, *m_networks = nullptr, *m_file = nullptr;
    QSpinBox *m_width = nullptr, *m_pixels = nullptr, *m_profileId = nullptr;
    QDoubleSpinBox *m_scale = nullptr, *m_reference = nullptr;
    QComboBox *m_address = nullptr, *m_model = nullptr, *m_assign = nullptr;
    QLabel *m_role = nullptr, *m_note = nullptr;
};

class ProfileInspector final : public QWidget, private AzToolsFramework::ToolsApplicationNotificationBus::Handler
{
public:
    explicit ProfileInspector(QWidget* parent = nullptr) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        m_entity = new QLabel(this);
        m_entity->setWordWrap(true);
        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_tree = new QTreeWidget(this);
        m_tree->setColumnCount(2);
        m_tree->setHeaderLabels({"Profile", "Value"});
        m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_tree->setWordWrap(true);
        auto* buttons = new QHBoxLayout;
        auto* create = new QPushButton("Create profile...", this);
        m_open = new QPushButton("Open profile file", this);
        m_reveal = new QPushButton("Show in folder", this);
        buttons->addWidget(create);
        buttons->addWidget(m_open);
        buttons->addWidget(m_reveal);
        buttons->addStretch();
        layout->addWidget(m_entity);
        layout->addWidget(m_status);
        layout->addWidget(m_tree, 1);
        layout->addLayout(buttons);
        m_progress = new QLabel(this);
        m_progress->setWordWrap(true);
        layout->addWidget(m_progress);
        connect(create, &QPushButton::clicked, this, [this] { CreateProfile(); });
        connect(m_open, &QPushButton::clicked, this, [this] { if (const auto f = SelectedFile(); !f.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(f)); });
        connect(m_reveal, &QPushButton::clicked, this, [this]
        { if (const auto f = SelectedFile(); !f.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(f).absolutePath())); });
        connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { UpdateButtons(); });
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { Refresh(); });
        timer->start(1000);
        AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusConnect();
        Refresh();
    }
    ~ProfileInspector() override { AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusDisconnect(); }

private:
    void AfterEntitySelectionChanged(const AzToolsFramework::EntityIdList&, const AzToolsFramework::EntityIdList&) override
    {
        QTimer::singleShot(0, this, [this] { Refresh(); });
    }
    QString SelectedFile() const
    {
        for (auto* item = m_tree->currentItem(); item; item = item->parent())
            if (const auto file = item->data(0, Qt::UserRole).toString(); !file.isEmpty()) return file;
        return {};
    }
    void UpdateButtons()
    {
        const bool file = !SelectedFile().isEmpty();
        m_open->setEnabled(file);
        m_reveal->setEnabled(file);
    }
    AZ::u32 SelectedProfileId() const
    {
        for (auto* item = m_tree->currentItem(); item; item = item->parent())
            if (const auto id = item->data(1, Qt::UserRole).toUInt(); id) return id;
        return 1;
    }
    void Refresh()
    {
        const AZ::EntityId mesh = SelectedMesh();
        AZStd::string report;
        if (mesh.IsValid()) MeshRequestBus::EventResult(report, mesh, &MeshRequests::GetProfileReportJson);
        const QString json = QString::fromUtf8(report.c_str());
        if (mesh == m_mesh && json == m_report) return;
        const bool sameMesh = mesh == m_mesh;
        m_mesh = mesh;
        m_report = json;
        if (!mesh.IsValid())
        {
            m_entity->setText("Select an entity with a SilPOM Mesh component.");
            m_status->clear();
            m_tree->clear();
            UpdateButtons();
            return;
        }
        AZStd::string name;
        AZ::ComponentApplicationBus::BroadcastResult(name, &AZ::ComponentApplicationRequests::GetEntityName, mesh);
        const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
        m_entity->setText(QString("<b>%1</b> - surface %2").arg(QString::fromUtf8(name.c_str()).toHtmlEscaped(),
            root["surface"].toString().isEmpty() ? "(none)" : root["surface"].toString().toHtmlEscaped()));
        m_status->setText("Status: " + root["status"].toString());
        // Keep expansion and selection across refreshes of the same entity.
        QSet<QString> expanded;
        QString current;
        if (sameMesh)
        {
            for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
                if (m_tree->topLevelItem(i)->isExpanded()) expanded.insert(m_tree->topLevelItem(i)->text(0));
            if (auto* item = m_tree->currentItem()) current = (item->parent() ? item->parent()->text(0) + "/" : QString()) + item->text(0);
        }
        m_tree->clear();
        for (const auto& value : root["profiles"].toArray())
        {
            const QJsonObject entry = value.toObject();
            const QJsonObject binding = entry["binding"].toObject();
            const QJsonObject profile = entry["profile"].toObject();
            const QString source = entry["source"].toString(), state = entry["state"].toString();
            auto* top = new QTreeWidgetItem(m_tree);
            const AZ::u32 id = AZ::u32(entry["id"].toInt());
            top->setText(0, QString("Profile %1").arg(id));
            top->setData(1, Qt::UserRole, id);
            QString summary = source == "none" ? "untrained (traversed relief from the binding)" : source == "base mesh" ? "base mesh"
                : QString("%1 profile, %2").arg(source, state);
            if (!profile.isEmpty()) summary += QString(" - model %1, %2").arg(profile["shaderModel"].toInt()).arg(RoleName(AZ::u32(profile["role"].toInt())));
            top->setText(1, summary);
            if (state == "not processed" || state == "invalid") top->setForeground(1, QBrush(QColor(230, 120, 90)));
            const auto add = [top](const QString& key, const QString& text, const QString& file = {})
            {
                auto* item = new QTreeWidgetItem(top, {key, text});
                item->setToolTip(1, text);
                if (!file.isEmpty()) item->setData(0, Qt::UserRole, file);
                return item;
            };
            if (!entry["product"].toString().isEmpty())
            {
                const QString file = SourceOfProduct(entry["product"].toString());
                add("Profile", file.isEmpty() ? entry["product"].toString() : QDir::toNativeSeparators(file), file);
                top->setData(0, Qt::UserRole, file);
            }
            if (!entry["error"].toString().isEmpty()) add("Error", entry["error"].toString());
            if (!profile.isEmpty())
            {
                const auto address = profile["addressMode"].toInt() ? "clamp" : "repeat";
                add("Calibration", QString("%1 m, reference %2, %3").arg(Number(profile["scale"].toDouble()), Number(profile["reference"].toDouble()), address));
                const QString height = profile["height"].toString();
                const QString heightFile = SourceOfProduct(height);
                add("Height image", height.isEmpty() ? QString("the binding's own (%1)").arg(binding["height"].toString())
                    : heightFile.isEmpty() ? height : QDir::toNativeSeparators(heightFile), heightFile);
                add("Trained size", QString("%1 x %2").arg(profile["width"].toInt()).arg(profile["heightPixels"].toInt()));
                const QString cache = profile["cache"].toString().isEmpty() ? binding["cache"].toString() : profile["cache"].toString();
                if (profile["shaderModel"].toInt() == 0) add("Seed cache", cache.isEmpty() ? "none (seed network)" : cache);
                add("Networks", NetworkList(AZ::u32(profile["flags"].toInt())));
                const QString networks = profile["networks"].toString();
                add("Weights from", networks.isEmpty() ? "reference networks (Tools/CompiledNetworks)" : "trained networks: " + networks);
                add("Generation", profile["generation"].toString().left(16));
            }
            if (!binding.isEmpty())
            {
                if (!binding["override"].toString().isEmpty()) add("Override", binding["override"].toString());
                if (source == "none" || source == "model 0 default")
                {
                    add("Binding", QString("model %1, %2 m, reference %3, %4").arg(binding["model"].toInt())
                        .arg(Number(binding["scale"].toDouble()), Number(binding["reference"].toDouble()), binding["addressMode"].toInt() ? "clamp" : "repeat"));
                    add("Binding height", binding["height"].toString().isEmpty() ? "(none)" : binding["height"].toString());
                }
            }
            else add("Binding", "none in this component (the surface names this ID)");
            top->setExpanded(!sameMesh || expanded.contains(top->text(0)));
        }
        if (!current.isEmpty())
            for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
                if (((*it)->parent() ? (*it)->parent()->text(0) + "/" : QString()) + (*it)->text(0) == current) m_tree->setCurrentItem(*it);
        UpdateButtons();
    }
    void CreateProfile()
    {
        CreateProfileDialog dialog(this, m_mesh, SelectedProfileId());
        if (dialog.exec() != QDialog::Accepted) return;
        const QString file = dialog.Profile();
        if (dialog.Assignment() == 2) return AssignToSurface(file, dialog.ProfileId());
        m_progress->setText("Wrote " + QDir::toNativeSeparators(file) + ". The Asset Processor builds it and writes its weights.");
        if (dialog.Assignment() == 1) WaitAndOverride(file, m_mesh, dialog.ProfileId(), 0);
    }
    // The override needs the product's asset ID, which exists once the Asset
    // Processor has built the new profile.
    void WaitAndOverride(const QString& file, AZ::EntityId mesh, AZ::u32 profileId, int attempt)
    {
        bool found = false;
        AZ::Data::AssetInfo info;
        AZStd::string watchFolder;
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(found, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourcePath,
            file.toUtf8().constData(), info, watchFolder);
        if (attempt == 0)
            AzFramework::AssetSystemRequestBus::Broadcast(&AzFramework::AssetSystem::AssetSystemRequests::EscalateAssetBySearchTerm,
                AZStd::string_view(file.toUtf8().constData()));
        AZ::Data::AssetInfo product;
        if (found)
        {
            const AZ::Data::AssetId id(info.m_assetId.m_guid, 0);
            AZ::Data::AssetCatalogRequestBus::BroadcastResult(product, &AZ::Data::AssetCatalogRequests::GetAssetInfoById, id);
        }
        if (product.m_assetId.IsValid())
        {
            AZStd::string error;
            if (AssignProfileOverride(mesh, profileId, product.m_assetId, error))
                m_progress->setText(QString("Assigned %1 to profile %2 (Trained profile override).").arg(QFileInfo(file).fileName()).arg(profileId));
            else m_progress->setText("Could not assign the profile: " + QString::fromUtf8(error.c_str()));
            return;
        }
        if (attempt >= 360)
        {
            m_progress->setText("The Asset Processor has not built " + QFileInfo(file).fileName()
                + " after 3 minutes; check it for errors, then set the Trained profile override by hand.");
            return;
        }
        m_progress->setText(QString("Waiting for the Asset Processor to build %1...").arg(QFileInfo(file).fileName()));
        QTimer::singleShot(500, this, [this, file, mesh, profileId, attempt] { WaitAndOverride(file, mesh, profileId, attempt + 1); });
    }
    // Names the profile in <surface>.silpomprofiles, next to the surface's
    // .silpom.json. The surface is rebuilt and every use of it picks it up.
    void AssignToSurface(const QString& file, AZ::u32 profileId)
    {
        AZ::Data::AssetId surface;
        const QString sidecar = GetMeshSurface(m_mesh, surface) ? SourceOfAsset(surface) : QString();
        if (!sidecar.endsWith(".silpom.json"))
        {
            m_progress->setText("Wrote the profile, but the surface's .silpom.json was not found; assign it with an override instead.");
            return;
        }
        const QString map = sidecar.left(sidecar.size() - int(strlen(".silpom.json"))) + ".silpomprofiles";
        QJsonObject profiles;
        if (QFile in(map); in.open(QIODevice::ReadOnly)) profiles = QJsonDocument::fromJson(in.readAll()).object();
        profiles[QString::number(profileId)] = QFileInfo(map).dir().relativeFilePath(file);
        QFile out(map);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        {
            m_progress->setText("Cannot write " + QDir::toNativeSeparators(map));
            return;
        }
        out.write(QJsonDocument(profiles).toJson(QJsonDocument::Indented));
        m_progress->setText(QString("Assigned %1 to profile %2 in %3. A Blender export of this surface rewrites that file from "
            "its own profile assignments.").arg(QFileInfo(file).fileName()).arg(profileId).arg(QFileInfo(map).fileName()));
    }

    AZ::EntityId m_mesh;
    QString m_report;
    QLabel *m_entity = nullptr, *m_status = nullptr, *m_progress = nullptr;
    QTreeWidget* m_tree = nullptr;
    QPushButton *m_open = nullptr, *m_reveal = nullptr;
};
} // namespace

void ProfileInspectorSystem::Reflect(AZ::ReflectContext* context)
{
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context)) sc->Class<ProfileInspectorSystem, AZ::Component>()->Version(1);
}
void ProfileInspectorSystem::Activate() { AzToolsFramework::EditorEvents::Bus::Handler::BusConnect(); }
void ProfileInspectorSystem::Deactivate() { AzToolsFramework::EditorEvents::Bus::Handler::BusDisconnect(); }
void ProfileInspectorSystem::NotifyRegisterViews()
{
    AzToolsFramework::ViewPaneOptions options;
    options.paneRect = QRect(80, 80, 560, 700);
    options.preferedDockingArea = Qt::RightDockWidgetArea;
    AzToolsFramework::RegisterViewPane<ProfileInspector>(ProfilePaneName, "Tools", options);
}
} // namespace SilPOM
