// SPDX-License-Identifier: MIT
#include "ProfileInspector.h"
#include <SilPOM/Mesh.h>
#include <SilPOM/SurfaceProfileAsset.h>
#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/Utils/Utils.h>
#include <AzFramework/Asset/AssetSystemBus.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <cstring>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QGroupBox>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QScrollArea>
#include <QCoreApplication>
#include <QSplitter>
#include <QTabWidget>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
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
#include <QSignalBlocker>
#include <QVector>
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

const char* RoleName(AZ::u32 role) { return role == 1 ? "Wall" : role == 2 ? "Opening" : "Surface"; }
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

// What training runs with: the engine's Python (its requirements include NumPy
// and Pillow) and the SilPOM tool libraries built next to the Editor.
// SILPOM_PYTHON and SILPOM_TOOLS_BIN override them.
QString TrainingPython()
{
    if (const QString python = QString::fromLocal8Bit(qgetenv("SILPOM_PYTHON")); !python.isEmpty()) return python;
    return QDir(QString::fromUtf8(AZ::Utils::GetEnginePath().c_str())).filePath("python/python.cmd");
}
QString TrainingTools()
{
    if (const QString tools = QString::fromLocal8Bit(qgetenv("SILPOM_TOOLS_BIN")); !tools.isEmpty()) return tools;
    return QCoreApplication::applicationDirPath();
}
constexpr const char* TrainingLibraries[] = {"silpom_welded_teacher.dll", "silpom_level_one.dll", "silpom_visibility.dll"};
QString GemFolder()
{
    AZStd::string found;
    if (auto* registry = AZ::SettingsRegistry::Get())
        AZ::SettingsRegistryMergeUtils::VisitActiveGems(*registry, [&found](AZStd::string_view name, AZStd::string_view path)
        { if (name == "SilPOM") found = path; });
    return QString::fromUtf8(found.c_str());
}

// Trains a surface profile for one profile ID of the selected mesh's surface,
// from that ID's own faces, height image and calibration (Tools/silpom_train.py).
class TrainProfileDialog final : public QDialog
{
public:
    TrainProfileDialog(QWidget* parent, const QJsonObject& entry, const QString& sidecar) : QDialog(parent), m_sidecar(sidecar)
    {
        m_id = AZ::u32(entry["id"].toInt());
        const QJsonObject binding = entry["binding"].toObject(), profile = entry["profile"].toObject();
        setWindowTitle(QString("Train SilPOM profile %1").arg(m_id));
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
        auto* about = new QLabel(QString("Trains networks for the faces of <b>%1</b> tagged with profile %2, on their height image "
            "at its calibration. Rays are cast at those faces and labelled with SilPOM's exact CPU intersection.")
            .arg(QFileInfo(sidecar).fileName().toHtmlEscaped()).arg(m_id), this);
        about->setWordWrap(true);
        // Height and displacement default to the mesh's own binding for this ID
        // (the prefab, from the Blender export), never to whatever profile is
        // assigned now; the current profile only fills in what the binding lacks.
        const bool trained = !profile.isEmpty();
        const bool fromBinding = !binding.isEmpty() && !binding["height"].toString().isEmpty();
        const QJsonObject calibration = fromBinding || !trained ? binding : profile;
        QString height = SourceOfProduct(calibration["height"].toString());
        m_height = new QLineEdit(QDir::toNativeSeparators(height), this);
        form->addRow("Height image", browseRow(m_height, [this]
        {
            const auto file = QFileDialog::getOpenFileName(this, "Height image", QFileInfo(m_height->text()).absolutePath(), "Height images (*.png)");
            if (!file.isEmpty()) m_height->setText(QDir::toNativeSeparators(file));
        }));
        m_scale = new QDoubleSpinBox(this); m_scale->setRange(0.0001, 10); m_scale->setDecimals(4); m_scale->setSingleStep(0.01); m_scale->setSuffix(" m");
        m_scale->setValue(calibration["scale"].toDouble(0.1));
        form->addRow("Displacement", m_scale);
        m_reference = new QDoubleSpinBox(this); m_reference->setRange(0, 1); m_reference->setDecimals(4); m_reference->setSingleStep(0.05);
        m_reference->setValue(calibration["reference"].toDouble(0.05));
        form->addRow("Reference height", m_reference);
        m_address = new QComboBox(this); m_address->addItems({"Repeat", "Clamp"});
        m_address->setCurrentIndex(calibration["addressMode"].toInt());
        form->addRow("Addressing", m_address);
        m_role = new QComboBox(this);
        m_role->addItem("Surface", "Surface");
        m_role->addItem("Wall", "Wall");
        m_role->addItem("Opening", "Opening");
        m_role->setItemData(0, "Any relief surface: floors, steps, terrain, panels.", Qt::ToolTipRole);
        m_role->setItemData(1, "Thick walls: wall hit records and backup shells keep their silhouette from far away.", Qt::ToolTipRole);
        m_role->setItemData(2, "Door and window trim and reveals: coverage follows the opening's edges.", Qt::ToolTipRole);
        m_role->setCurrentIndex(trained ? int(profile["role"].toInt()) : 0);
        form->addRow("Role", m_role);
        m_set = new QComboBox(this);
        m_set->addItem("Hit network", "hit");
        m_set->addItem("Hit network, Level 2 and visibility", "full");
        m_set->addItem("Model 0: seed and no-march hit networks", "model0");
        m_set->setItemData(0, "Predicts where each ray hits the relief; the shader refines and checks it against the height image.", Qt::ToolTipRole);
        m_set->setItemData(1, "Adds a second candidate predictor and neural visibility (needed for neural-only shadows).", Qt::ToolTipRole);
        m_set->setItemData(2, "A seed network (and, for a 6656 x 6144 image, a baked seed cache) and a no-march hit network, trained on this profile ID's faces.", Qt::ToolTipRole);
        m_set->setCurrentIndex(trained && (AZ::u32(profile["flags"].toInt()) & SurfaceProfileAsset::LevelTwoFlag) ? 1 : 0);
        form->addRow("Networks", m_set);
        m_quality = new QComboBox(this);
        m_quality->addItem("Quick (fewer rays and steps; seconds)", true);
        m_quality->addItem("Full (230,000 rays, 3 rounds; about a minute)", false);
        form->addRow("Training", m_quality);
        const QString existing = SourceOfProduct(entry["product"].toString());
        const QFileInfo surface(sidecar);
        const QString stem = surface.fileName().left(surface.fileName().size() - int(strlen(".silpom.json")));
        QString file = surface.dir().filePath(QString("profiles/%1-%2.silpomprofile").arg(stem).arg(m_id));
        if (!existing.isEmpty())
        {
            const QFileInfo current(existing);
            file = current.dir().filePath(current.completeBaseName() + "-trained.silpomprofile");
        }
        const QString base = file.left(file.size() - int(strlen(".silpomprofile")));
        for (int n = 2; QFileInfo::exists(file); ++n) file = QString("%1-%2.silpomprofile").arg(base).arg(n);
        m_file = new QLineEdit(QDir::toNativeSeparators(file), this);
        form->addRow("Profile file", browseRow(m_file, [this]
        {
            const auto file = QFileDialog::getSaveFileName(this, "Profile file", m_file->text(), "SilPOM surface profiles (*.silpomprofile)");
            if (!file.isEmpty()) m_file->setText(QDir::toNativeSeparators(file.endsWith(".silpomprofile") ? file : file + ".silpomprofile"));
        }));
        m_assign = new QComboBox(this);
        m_assign->addItem("This entity (Trained profile override)");
        m_assign->addItem("The surface, for every use (.silpomprofiles)");
        m_assign->addItem("Do not assign");
        m_assign->setCurrentIndex(0);  // an undoable prefab edit
        form->addRow("Assign to", m_assign);
        m_note = new QLabel(this);
        m_note->setWordWrap(true);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        m_train = buttons->button(QDialogButtonBox::Ok);
        m_train->setText("Train");
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { Accept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(about);
        layout->addLayout(form);
        layout->addWidget(m_note);
        layout->addWidget(buttons);
        connect(m_role, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { Update(); });
        connect(m_set, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { Update(); });
        connect(m_file, &QLineEdit::textChanged, this, [this] { Update(); });
        Update();
        resize(680, sizeHint().height());
    }
    AZ::u32 ProfileId() const { return m_id; }
    QString Profile() const { return QDir::fromNativeSeparators(m_file->text()); }
    int Assignment() const { return m_assign->currentIndex(); }  // 0 override, 1 surface, 2 none
    QString Python() const { return TrainingPython(); }
    QStringList Arguments() const
    {
        QStringList args{QDir(GemFolder()).filePath("Tools/silpom_train.py"),
            "--surface", m_sidecar, "--profile-id", QString::number(m_id), "--height", QDir::fromNativeSeparators(m_height->text()),
            "--scale", Number(m_scale->value()), "--reference", Number(m_reference->value()),
            "--address", m_address->currentIndex() ? "clamp" : "repeat", "--role", m_role->currentData().toString(),
            "--networks", m_set->currentData().toString(), "--tools", TrainingTools(),
            "--profile", Profile()};
        if (m_quality->currentData().toBool()) args << "--quick";
        return args;
    }

private:
    bool Available(QString* = nullptr) const { return true; }  // every role has every network set
    void Update()
    {
        QString why;
        const bool available = Available(&why);
        m_train->setEnabled(available);
        QString note = available ? QString() : why;
        if (available && QFileInfo::exists(QDir::fromNativeSeparators(m_file->text())))
            note = "Training replaces this profile and its weights file.";
        m_note->setText(note);
    }
    void Accept()
    {
        const auto fail = [this](const QString& message) { QMessageBox::warning(this, windowTitle(), message); };
        if (!QFileInfo::exists(QDir::fromNativeSeparators(m_height->text()))) return fail("The height image does not exist.");
        if (!Profile().endsWith(".silpomprofile")) return fail("Choose a profile file ending in .silpomprofile.");
        if (!QFileInfo::exists(TrainingPython())) return fail("The engine's Python was not found: " + QDir::toNativeSeparators(TrainingPython()));
        for (const char* library : TrainingLibraries)
            if (!QDir(TrainingTools()).exists(library))
                return fail(QString("%1 is missing next to the Editor. Build the SilPOM.Editor target again; it builds SilPOM's "
                    "training libraries.").arg(library));
        if (GemFolder().isEmpty()) return fail("Cannot locate the SilPOM gem.");
        if (QFileInfo::exists(Profile()) && QMessageBox::question(this, windowTitle(),
                QString("%1 already exists. Training replaces it and its weights for every prefab that uses it. Continue?")
                    .arg(QFileInfo(Profile()).fileName())) != QMessageBox::Yes)
            return;
        if (Assignment() == 1 && QMessageBox::question(this, windowTitle(),
                "Assigning to the surface changes this profile ID for every prefab that uses the surface. Continue?") != QMessageBox::Yes)
            return;
        QDir().mkpath(QFileInfo(Profile()).absolutePath());
        accept();
    }

    QString m_sidecar;
    AZ::u32 m_id = 0;
    QLineEdit *m_height = nullptr, *m_file = nullptr;
    QDoubleSpinBox *m_scale = nullptr, *m_reference = nullptr;
    QComboBox *m_address = nullptr, *m_role = nullptr, *m_set = nullptr, *m_quality = nullptr, *m_assign = nullptr;
    QLabel* m_note = nullptr;
    QPushButton* m_train = nullptr;
};

// What a profile was trained on: Tools/silpom_train.py's images and report.
class TrainingView final : public QTabWidget
{
public:
    explicit TrainingView(QWidget* parent) : QTabWidget(parent) {}
    // The training folder of a profile: its "Networks" folder's parent.
    static QString FolderOfProfile(const QString& profile)
    {
        QFile file(profile);
        if (!file.open(QIODevice::ReadOnly)) return {};
        const QString networks = QJsonDocument::fromJson(file.readAll()).object()["Networks"].toString();
        if (networks.isEmpty()) return {};
        const QString folder = QFileInfo(QFileInfo(profile).dir().filePath(networks)).dir().absolutePath();
        return QFileInfo::exists(QDir(folder).filePath("visuals")) ? folder : QString();
    }
    void Show(const QString& folder)
    {
        if (folder == m_folder) return;
        m_folder = folder;
        clear();
        if (folder.isEmpty()) return;
        const QDir visuals(QDir(folder).filePath("visuals"));
        const struct { const char* file; const char* title; } pages[] = {
            {"geometry.png", "Surface"}, {"surface.png", "Relief"}, {"rays.png", "Rays"}, {"angles.png", "Directions"},
            {"errors.png", "Trained"}, {"errors-reference.png", "Reference"}};
        for (const auto& page : pages)
        {
            const QPixmap image(visuals.filePath(page.file));
            if (image.isNull()) continue;
            auto* label = new QLabel;
            label->setPixmap(image.scaledToWidth(qMin(image.width(), 640), Qt::SmoothTransformation));
            label->setAlignment(Qt::AlignCenter);
            auto* scroll = new QScrollArea;
            scroll->setWidget(label);
            scroll->setWidgetResizable(true);
            addTab(scroll, page.title);
        }
        auto* summary = new QLabel(Summary(QDir(folder).filePath("report.json")));
        summary->setWordWrap(true);
        summary->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* scroll = new QScrollArea;
        scroll->setWidget(summary);
        scroll->setWidgetResizable(true);
        insertTab(0, scroll, "Summary");
        setCurrentIndex(0);
    }

private:
    static QString Summary(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return "No training report.";
        const QJsonObject r = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonObject calibration = r["calibration"].toObject();
        QStringList lines{QString("<b>Profile %1</b> of %2: %3 faces, %4 x %5 height image")
            .arg(r["profile_id"].toInt()).arg(QFileInfo(r["surface"].toString()).fileName().toHtmlEscaped()).arg(r["faces"].toInt())
            .arg(r["height_size"].toArray()[0].toInt()).arg(r["height_size"].toArray()[1].toInt()),
            QString("Calibration: %1 m, reference %2, %3. Role %4, networks %5, %6 training, %7 minutes.")
            .arg(Number(calibration["scale"].toDouble()), Number(calibration["reference"].toDouble()), calibration["address"].toString(),
                 r["role"].toString(), r["networks"].toString(), r["preset"].toString(), QString::number(r["seconds"].toDouble() / 60, 'f', 1))};
        const auto hit = [](const QJsonObject& network)
        {
            const QJsonObject all = network["held_out"].toObject()["all"].toObject();
            return QString("%1% of held-out hits seeded within 1 mm, median seed error %2 mm, hit recall %3%")
                .arg(Number(network["within_1mm"].toDouble() * 100)).arg(Number(all["seed_p50_mm"].toDouble()))
                .arg(Number(all["hit_recall_at_005"].toDouble() * 100));
        };
        if (r.contains("hit_network"))
        {
            lines << "<b>Trained hit network:</b> " + hit(r["hit_network"].toObject());
            const QJsonObject solver = r["hit_network"].toObject()["solver_held_out"].toObject();
            lines << QString("In the solver: %1 of %2 hits uncovered, %3 base-plane fills.")
                .arg(solver["uncovered_teacher_hits"].toInt()).arg(solver["teacher_hits"].toInt()).arg(solver["fallback_fills"].toInt());
            lines << "<b>Reference network (trained for another image), on these rays:</b> " + hit(r["reference_hit_network"].toObject());
            const QJsonObject reference = r["reference_hit_network"].toObject()["solver_held_out"].toObject();
            if (!reference.isEmpty())
                lines << QString("Reference in the solver: %1 of %2 hits uncovered, %3 base-plane fills.")
                    .arg(reference["uncovered_teacher_hits"].toInt()).arg(reference["teacher_hits"].toInt()).arg(reference["fallback_fills"].toInt());
            const QJsonArray budget = r["solver_budget"].toArray();
            if (budget.size() == 3)
                lines << QString("Solver budget: %1 iterations, %2 retries, tolerance x%3 (the mesh's). Round %4 of %5 selected.")
                    .arg(budget[0].toInt()).arg(budget[1].toInt()).arg(Number(budget[2].toDouble()))
                    .arg(r["hit_network"].toObject()["selected_round"].toInt()).arg(r["hit_network"].toObject()["rounds"].toArray().size());
        }
        if (r.contains("physical_visibility"))
        {
            const QJsonObject held = r["physical_visibility"].toObject()["held_out"].toObject();
            lines << QString("<b>Physical visibility:</b> %1 wrongly lit and %2 wrongly shadowed of %3 held-out queries.")
                .arg(held["method_false_lit"].toInt()).arg(held["method_false_shadow"].toInt()).arg(held["rays"].toInt());
        }
        if (r.contains("model_zero"))
        {
            const QJsonObject seed = r["model_zero"].toObject()["seed_network"].toObject()["held_out"].toObject();
            lines << QString("<b>Model 0 seed network:</b> median error %1 mm, %2% within 4 mm.")
                .arg(Number(seed["seed_error_p50_mm"].toDouble())).arg(Number(seed["within_4mm"].toDouble() * 100));
        }
        return lines.join("<br><br>");
    }
    QString m_folder;
};

// Picks an existing .silpomprofile for one profile ID: those near the surface,
// or any other file, applied as an override or for every use of the surface.
class AssignProfileDialog final : public QDialog
{
public:
    AssignProfileDialog(QWidget* parent, const QJsonObject& entry, const QString& sidecar) : QDialog(parent)
    {
        m_id = AZ::u32(entry["id"].toInt());
        setWindowTitle(QString("Assign a SilPOM profile to profile %1").arg(m_id));
        m_current = QDir::cleanPath(SourceOfProduct(entry["product"].toString()));
        auto* about = new QLabel(QString("Profiles found next to <b>%1</b>. The selected one replaces what profile %2 uses now.")
            .arg(QFileInfo(sidecar).fileName().toHtmlEscaped()).arg(m_id), this);
        about->setWordWrap(true);
        m_list = new QTreeWidget(this);
        m_list->setHeaderLabels({"Profile", "Role", "Networks", "Weights", "Modified"});
        m_list->setRootIsDecorated(false);
        m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        QDirIterator files(QFileInfo(sidecar).absolutePath(), {"*.silpomprofile"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext())
        {
            const QString file = files.next();
            if (!file.contains(".training/")) Add(file);
        }
        auto* browse = new QPushButton("Other profile...", this);
        connect(browse, &QPushButton::clicked, this, [this, sidecar]
        {
            const auto file = QFileDialog::getOpenFileName(this, "SilPOM profile", QFileInfo(sidecar).absolutePath(),
                "SilPOM surface profiles (*.silpomprofile)");
            if (!file.isEmpty()) m_list->setCurrentItem(Add(file));
        });
        m_assign = new QComboBox(this);
        m_assign->addItem("This entity (Trained profile override)");
        m_assign->addItem("The surface, for every use (.silpomprofiles)");
        auto* target = new QFormLayout;
        target->addRow("Assign to", m_assign);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        m_ok = buttons->button(QDialogButtonBox::Ok);
        m_ok->setText("Assign");
        connect(buttons, &QDialogButtonBox::accepted, this, [this]
        {
            if (Assignment() == 1 && QMessageBox::question(this, windowTitle(),
                    "Assigning to the surface changes this profile ID for every prefab that uses the surface. Continue?") != QMessageBox::Yes)
                return;
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_list, &QTreeWidget::currentItemChanged, this, [this] { m_ok->setEnabled(!Profile().isEmpty()); });
        connect(m_list, &QTreeWidget::itemDoubleClicked, this, [buttons] { emit buttons->accepted(); });
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(about);
        layout->addWidget(m_list, 1);
        layout->addWidget(browse, 0, Qt::AlignLeft);
        layout->addLayout(target);
        layout->addWidget(buttons);
        m_ok->setEnabled(false);
        resize(820, 420);
    }
    AZ::u32 ProfileId() const { return m_id; }
    QString Profile() const { return m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole).toString() : QString(); }
    int Assignment() const { return m_assign->currentIndex(); }  // 0 override, 1 surface

private:
    QTreeWidgetItem* Add(const QString& path)
    {
        const QString file = QDir::cleanPath(path);
        for (int i = 0; i < m_list->topLevelItemCount(); ++i)
            if (m_list->topLevelItem(i)->data(0, Qt::UserRole).toString() == file) return m_list->topLevelItem(i);
        QFile in(file);
        const QJsonObject profile = in.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(in.readAll()).object() : QJsonObject();
        const QFileInfo info(file);
        const QString weightsName = profile["Weights"].toString().isEmpty()
            ? info.fileName().left(info.fileName().size() - int(strlen(".silpomprofile"))) + ".silpomweights.json" : profile["Weights"].toString();
        const QFileInfo weightsFile(info.dir().filePath(weightsName));
        QFile weightsIn(weightsFile.filePath());
        const QJsonObject weights = weightsIn.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(weightsIn.readAll()).object() : QJsonObject();
        const QString networks = weights.contains("Seed") ? "model 0 (seed, no-march hit)"
            : weights.contains("LevelTwo") ? "hit, Level 2, visibility" : weights.isEmpty() ? "(weights not written yet)" : "hit";
        const QString origin = profile.contains("Networks") ? "trained for this surface" : "reference networks";
        auto* item = new QTreeWidgetItem(m_list, {info.fileName() + (file == m_current ? "  (current)" : QString()),
            profile["Role"].toString(), networks, origin,
            (weightsFile.exists() ? weightsFile : info).lastModified().toString("yyyy-MM-dd HH:mm")});
        item->setData(0, Qt::UserRole, file);
        item->setToolTip(0, QDir::toNativeSeparators(file));
        return item;
    }

    AZ::u32 m_id = 0;
    QString m_current;
    QTreeWidget* m_list = nullptr;
    QComboBox* m_assign = nullptr;
    QPushButton* m_ok = nullptr;
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
        m_tree->setMinimumHeight(140);
        auto* buttons = new QHBoxLayout;
        m_trainButton = new QPushButton("Train profile...", this);
        m_trainButton->setToolTip("Train networks for the selected profile ID from this surface's own faces and height image.");
        m_open = new QPushButton("Open profile file", this);
        m_reveal = new QPushButton("Show in folder", this);
        buttons->addWidget(m_trainButton);
        m_assignButton = new QPushButton("Assign profile...", this);
        m_assignButton->setToolTip("Use an existing trained profile for the selected profile ID.");
        m_clearButton = new QPushButton("Clear override", this);
        m_clearButton->setToolTip("Remove this entity's Trained profile override; the ID goes back to the surface's profile.");
        buttons->addWidget(m_assignButton);
        buttons->addWidget(m_clearButton);
        buttons->addWidget(m_open);
        buttons->addWidget(m_reveal);
        buttons->addStretch();
        m_progress = new QLabel(this);
        m_progress->setWordWrap(true);
        m_bar = new QProgressBar(this);
        m_bar->setRange(0, 100);
        m_bar->hide();
        m_cancel = new QPushButton("Cancel training", this);
        m_cancel->hide();
        auto* job = new QHBoxLayout;
        job->addWidget(m_bar, 1);
        job->addWidget(m_cancel);
        m_view = new TrainingView(this);
        auto* top = new QWidget(this);
        auto* topLayout = new QVBoxLayout(top);
        topLayout->setContentsMargins(0, 0, 0, 0);
        topLayout->addWidget(m_entity);
        topLayout->addWidget(m_status);
        topLayout->addWidget(m_tree, 1);
        topLayout->addLayout(buttons);
        topLayout->addLayout(job);
        topLayout->addWidget(m_progress);
        auto* split = new QSplitter(Qt::Vertical, this);
        split->addWidget(top);
        split->addWidget(m_view);
        split->setStretchFactor(0, 1);
        split->setStretchFactor(1, 2);
        layout->addWidget(split, 1);
        connect(m_trainButton, &QPushButton::clicked, this, [this] { TrainProfile(); });
        connect(m_assignButton, &QPushButton::clicked, this, [this] { AssignProfile(); });
        connect(m_clearButton, &QPushButton::clicked, this, [this] { ClearOverride(); });
        connect(m_cancel, &QPushButton::clicked, this, [this] { if (m_job) m_job->kill(); });
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
    ~ProfileInspector() override
    {
        AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusDisconnect();
        if (m_job) { m_job->disconnect(); m_job->kill(); m_job->waitForFinished(2000); }
    }

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
        const QString file = SelectedFile();
        m_open->setEnabled(!file.isEmpty());
        m_reveal->setEnabled(!file.isEmpty());
        const QJsonObject entry = SelectedEntry();
        const bool editable = m_mesh.IsValid() && !m_job && entry.contains("id") && entry["source"].toString() != "base mesh";
        m_trainButton->setEnabled(editable);
        m_assignButton->setEnabled(editable);
        m_clearButton->setEnabled(m_mesh.IsValid() && !m_job && !entry["binding"].toObject()["override"].toString().isEmpty());
        // What the selected profile was trained on, if SilPOM's trainer made it.
        if (!m_job) m_view->Show(file.endsWith(".silpomprofile") ? TrainingView::FolderOfProfile(file) : QString());
    }
    QJsonObject SelectedEntry() const
    {
        const AZ::u32 id = SelectedProfileId();
        for (const auto& value : QJsonDocument::fromJson(m_report.toUtf8()).object()["profiles"].toArray())
            if (AZ::u32(value.toObject()["id"].toInt()) == id) return value.toObject();
        return {};
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
        // The status can be long (generation hashes); the first clause, full text on hover.
        const QString status = root["status"].toString();
        m_status->setText("Status: " + status.section(';', 0, 0));
        m_status->setToolTip(status);
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
    QString SurfaceSidecar() const
    {
        AZ::Data::AssetId surface;
        const QString sidecar = GetMeshSurface(m_mesh, surface) ? SourceOfAsset(surface) : QString();
        return sidecar.endsWith(".silpom.json") ? sidecar : QString();
    }
    void AssignProfile()
    {
        const QString sidecar = SurfaceSidecar();
        if (sidecar.isEmpty()) { m_progress->setText("The surface's .silpom.json source was not found."); return; }
        auto* dialog = new AssignProfileDialog(this, SelectedEntry(), sidecar);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        const AZ::EntityId mesh = m_mesh;
        connect(dialog, &QDialog::accepted, this, [this, dialog, mesh]
        {
            if (dialog->Assignment() == 1) AssignToSurface(dialog->Profile(), dialog->ProfileId());
            else WaitAndOverride(dialog->Profile(), mesh, dialog->ProfileId(), 0);
        });
        dialog->open();
    }
    void ClearOverride()
    {
        const AZ::u32 id = SelectedProfileId();
        AZStd::string error;
        if (AssignProfileOverride(m_mesh, id, AZ::Data::AssetId(), error))
            m_progress->setText(QString("Cleared the override of profile %1; it uses the surface's profile again.").arg(id));
        else m_progress->setText("Could not clear the override: " + QString::fromUtf8(error.c_str()));
    }
    void TrainProfile()
    {
        AZ::Data::AssetId surface;
        const QString sidecar = GetMeshSurface(m_mesh, surface) ? SourceOfAsset(surface) : QString();
        if (!sidecar.endsWith(".silpom.json"))
        {
            m_progress->setText("The surface's .silpom.json source was not found; training needs the surface's faces.");
            return;
        }
        auto* dialog = new TrainProfileDialog(this, SelectedEntry(), sidecar);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        const AZ::EntityId mesh = m_mesh;
        connect(dialog, &QDialog::accepted, this, [this, dialog, mesh]
        {
            // Select and measure the networks with the solver budget this mesh renders with.
            QStringList arguments = dialog->Arguments();
            const QJsonArray budget = QJsonDocument::fromJson(m_report.toUtf8()).object()["validation"].toArray();
            if (budget.size() == 3)
                arguments << "--budget" << QString("%1,%2,%3").arg(budget[0].toInt()).arg(budget[1].toInt()).arg(Number(budget[2].toDouble()));
            StartJob(dialog->Python(), arguments, dialog->Profile(), dialog->ProfileId(), dialog->Assignment(), mesh);
        });
        dialog->open();
    }
    void StartJob(const QString& python, const QStringList& arguments, const QString& profile, AZ::u32 profileId,
        int assignment, AZ::EntityId mesh)
    {
        m_job = new QProcess(this);
        m_job->setProgram(python);
        m_job->setArguments(arguments);
        m_job->setProcessChannelMode(QProcess::MergedChannels);
        m_log.clear();
        m_bar->setValue(0);
        m_bar->show();
        m_cancel->show();
        m_view->Show({});
        m_progress->setText(QString("Training profile %1...").arg(profileId));
        connect(m_job, &QProcess::readyReadStandardOutput, this, [this]
        {
            while (m_job->canReadLine())
            {
                const QString line = QString::fromUtf8(m_job->readLine()).trimmed();
                m_log << line;
                if (line.startsWith("PROGRESS "))
                {
                    const int space = line.indexOf(' ', 9);
                    m_bar->setValue(line.mid(9, space - 9).toInt());
                    m_progress->setText(line.mid(space + 1));
                }
            }
        });
        connect(m_job, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, profile, profileId, assignment, mesh](int code, QProcess::ExitStatus status)
        {
            m_log << QString::fromUtf8(m_job->readAll()).trimmed();
            m_job->deleteLater();
            m_job = nullptr;
            m_bar->hide();
            m_cancel->hide();
            UpdateButtons();
            if (status != QProcess::NormalExit || code != 0)
            {
                QStringList tail = m_log.mid(qMax(0, m_log.size() - 12));
                m_progress->setText(QString("Training stopped (exit %1):\n%2").arg(code).arg(tail.join("\n")));
                return;
            }
            m_view->Show(TrainingView::FolderOfProfile(profile));
            if (assignment == 1) AssignToSurface(profile, profileId);
            else if (assignment == 0) WaitAndOverride(profile, mesh, profileId, 0);
            else m_progress->setText("Trained " + QDir::toNativeSeparators(profile) + ".");
        });
        connect(m_job, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
        {
            if (error == QProcess::FailedToStart && m_job)
            {
                m_progress->setText("Could not start Python: " + m_job->program());
                m_job->deleteLater();
                m_job = nullptr;
                m_bar->hide();
                m_cancel->hide();
                UpdateButtons();
            }
        });
        m_job->start();
        UpdateButtons();
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
    QPushButton *m_open = nullptr, *m_reveal = nullptr, *m_trainButton = nullptr, *m_cancel = nullptr;
    QPushButton *m_assignButton = nullptr, *m_clearButton = nullptr;
    QProgressBar* m_bar = nullptr;
    TrainingView* m_view = nullptr;
    QProcess* m_job = nullptr;
    QStringList m_log;
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
