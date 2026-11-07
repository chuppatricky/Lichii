// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/testbuild_updater.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/testbuild_release.h"

namespace {

#ifdef _WIN32
constexpr bool IsWindows = true;
#else
constexpr bool IsWindows = false;
#endif

QString ReleasesUrl() {
    // EDEN_NX2_UPDATE_URL points the check somewhere else (testing).
    const QString custom = qEnvironmentVariable("EDEN_NX2_UPDATE_URL");
    if (!custom.isEmpty()) {
        return custom;
    }
    return QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30")
        .arg(QString::fromUtf8(TestbuildRelease::Repository.data(),
                               static_cast<qsizetype>(TestbuildRelease::Repository.size())));
}

QNetworkRequest MakeRequest(const QUrl& url) {
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Lichii-updater"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

/// Windows: the folder the new build's folder is unpacked into, next to this build's folder when
/// that can be written, else Downloads.
QString WindowsInstallRoot() {
    QDir app_dir{QCoreApplication::applicationDirPath()};
    if (app_dir.cdUp() && QFileInfo{app_dir.absolutePath()}.isWritable()) {
        return app_dir.absolutePath();
    }
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

QString MiB(qint64 bytes) {
    return QString::number((bytes + (1 << 19)) >> 20);
}

} // namespace

TestbuildUpdater::TestbuildUpdater(QWidget* parent, std::function<bool()> is_busy_)
    : QObject{parent}, parent_widget{parent}, is_busy{std::move(is_busy_)},
      network{new QNetworkAccessManager(this)} {}

TestbuildUpdater::~TestbuildUpdater() = default;

void TestbuildUpdater::Check() {
    const auto version = TestbuildRelease::Version(Common::g_build_version);
    if (!version) {
        LOG_INFO(Frontend, "Update check skipped: {} isn't a chuppatricky-amd-testbuild release",
                 Common::g_build_version);
        return;
    }
    current_version = *version;
    QNetworkRequest request = MakeRequest(QUrl{ReleasesUrl()});
    request.setTransferTimeout(15000);
    QNetworkReply* reply = network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { OnReleases(reply); });
}

void TestbuildUpdater::OnReleases(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Frontend, "Update check failed: {}", reply->errorString().toStdString());
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    if (!document.isArray()) {
        LOG_WARNING(Frontend, "Update check failed: unexpected reply from GitHub");
        return;
    }
    std::optional<Update> best;
    std::vector<std::pair<u32, QString>> changes; // every newer release's notes
    for (const QJsonValue& value : document.array()) {
        const QJsonObject release = value.toObject();
        if (release.value(QStringLiteral("draft")).toBool()) {
            continue;
        }
        const QString tag = release.value(QStringLiteral("tag_name")).toString();
        const auto version = TestbuildRelease::Version(tag.toStdString());
        if (!version || *version <= current_version) {
            continue;
        }
        changes.emplace_back(*version, release.value(QStringLiteral("body")).toString());
        if (best && *version <= best->version) {
            continue;
        }
        for (const QJsonValue& asset_value : release.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject asset = asset_value.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (!TestbuildRelease::IsPlatformAsset(name.toStdString(), IsWindows)) {
                continue;
            }
            best = Update{
                .version = *version,
                .tag = tag,
                .page = QUrl{release.value(QStringLiteral("html_url")).toString()},
                .notes = release.value(QStringLiteral("body")).toString(),
                .asset_name = name,
                .asset_url = QUrl{asset.value(QStringLiteral("browser_download_url")).toString()},
                .asset_size = static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble()),
            };
            break;
        }
    }
    if (!best) {
        LOG_INFO(Frontend, "Update check: v{} is the newest chuppatricky-amd-testbuild",
                 current_version);
        return;
    }
    LOG_INFO(Frontend, "Update check: v{} is available ({})", best->version,
             best->asset_name.toStdString());
    std::sort(changes.begin(), changes.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    best->notes.clear();
    for (const auto& [version, body] : changes) {
        best->notes += QStringLiteral("# v%1\n\n%2\n\n").arg(version).arg(
            body.trimmed().isEmpty() ? tr("No notes for this version.") : body.trimmed());
    }
    pending = std::move(best);
    if (!is_busy()) {
        Ask();
    }
}

void TestbuildUpdater::ShowPending() {
    if (pending && !asked && !is_busy()) {
        Ask();
    }
}

void TestbuildUpdater::Ask() {
    if (!pending || asked) {
        return;
    }
    asked = true;
    const Update& update = *pending;
    QDialog dialog{parent_widget};
    dialog.setWindowTitle(tr("Update available"));
    dialog.resize(640, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* heading = new QLabel(tr("<b>chuppatricky-amd-testbuild v%1</b> is available. You have v%2.")
                                   .arg(update.version)
                                   .arg(current_version));
    layout->addWidget(heading);
    layout->addWidget(new QLabel(tr("What's new:")));
    auto* changelog = new QTextBrowser();
    changelog->setOpenExternalLinks(true);
    changelog->setMarkdown(update.notes);
    layout->addWidget(changelog, 1);
    auto* how = new QLabel(
        IsWindows ? tr("Updating downloads it (%1 MiB), unpacks it into its own folder next to "
                       "this one and starts it. Your settings, saves and shaders carry over; "
                       "this version's folder stays where it is.")
                        .arg(MiB(update.asset_size))
                  : tr("Updating downloads the new AppImage (%1 MiB) next to this one and starts "
                       "it. Your settings, saves and shaders carry over; this AppImage stays "
                       "where it is.")
                        .arg(MiB(update.asset_size)));
    how->setWordWrap(true);
    layout->addWidget(how);
    auto* buttons = new QDialogButtonBox();
    QPushButton* update_button =
        buttons->addButton(tr("Update to v%1").arg(update.version), QDialogButtonBox::AcceptRole);
    buttons->addButton(tr("Stay on v%1").arg(current_version), QDialogButtonBox::RejectRole);
    update_button->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        LOG_INFO(Frontend, "Update to v{} declined", update.version);
        return;
    }

    if (IsWindows && QDir{QCoreApplication::applicationDirPath()}.exists(QStringLiteral("user"))) {
        // A portable install keeps everything in "user" next to lichii.exe; the new folder
        // wouldn't have it.
        OpenPage(tr("This copy keeps its settings and saves in the \"user\" folder next to "
                    "lichii.exe. Download the new version and move that folder into it."));
        return;
    }
    if (!IsWindows && qEnvironmentVariable("APPIMAGE").isEmpty()) {
        OpenPage(tr("This copy isn't running from an AppImage, so it can't update itself."));
        return;
    }
    // Downloaded on an earlier launch: start it.
    const QFileInfo installed{InstalledProgram()};
    if (installed.exists() && (IsWindows || installed.size() == update.asset_size)) {
        LaunchAndQuit(installed.absoluteFilePath());
        return;
    }
    StartDownload();
}

QString TestbuildUpdater::DownloadPath() const {
    if (IsWindows) {
        return QDir::temp().filePath(pending->asset_name);
    }
    return QFileInfo{qEnvironmentVariable("APPIMAGE")}.absoluteDir().filePath(pending->asset_name);
}

QString TestbuildUpdater::InstalledProgram() const {
    if (IsWindows) {
        QString folder = pending->asset_name;
        folder.chop(4); // ".zip": the zip holds one folder named like it
        return QDir{WindowsInstallRoot()}.filePath(folder + QStringLiteral("/lichii.exe"));
    }
    return DownloadPath();
}

void TestbuildUpdater::StartDownload() {
    const QString path = DownloadPath();
    download_file = new QSaveFile(path, this);
    if (!download_file->open(QIODevice::WriteOnly)) {
        const QString error = download_file->errorString();
        download_file->deleteLater();
        download_file = nullptr;
        OpenPage(tr("Couldn't write %1: %2").arg(path, error));
        return;
    }
    progress = new QProgressDialog(
        tr("Downloading chuppatricky-amd-testbuild v%1...").arg(pending->version), tr("Cancel"), 0,
        100, parent_widget);
    progress->setWindowTitle(tr("Updating"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);

    download = network->get(MakeRequest(pending->asset_url));
    connect(download, &QNetworkReply::readyRead, this, [this] {
        if (download_file) {
            download_file->write(download->readAll());
        }
    });
    connect(download, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total <= 0) {
            total = pending->asset_size;
        }
        if (progress && total > 0) {
            progress->setValue(static_cast<int>(got * 100 / total));
        }
    });
    connect(progress, &QProgressDialog::canceled, download, &QNetworkReply::abort);
    connect(download, &QNetworkReply::finished, this, &TestbuildUpdater::OnDownloadFinished);
}

void TestbuildUpdater::OnDownloadFinished() {
    QNetworkReply* reply = download;
    download = nullptr;
    reply->deleteLater();
    if (progress) {
        progress->deleteLater();
        progress = nullptr;
    }
    QSaveFile* file = download_file;
    download_file = nullptr;
    file->deleteLater();

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        file->cancelWriting();
        LOG_INFO(Frontend, "Update download canceled");
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        file->cancelWriting();
        OpenPage(tr("The download failed: %1").arg(reply->errorString()));
        return;
    }
    file->write(reply->readAll());
    const qint64 written = file->size();
    if (pending->asset_size > 0 && written != pending->asset_size) {
        file->cancelWriting();
        OpenPage(tr("The download was incomplete (%1 of %2 bytes).")
                     .arg(written)
                     .arg(pending->asset_size));
        return;
    }
    if (!file->commit()) {
        OpenPage(tr("Couldn't save the download: %1").arg(file->errorString()));
        return;
    }
    LOG_INFO(Frontend, "Downloaded {} ({} bytes)", file->fileName().toStdString(), written);
    Install(file->fileName());
}

void TestbuildUpdater::Install(const QString& downloaded) {
    if (!IsWindows) {
        QFile::setPermissions(downloaded, QFile::permissions(downloaded) | QFile::ExeOwner |
                                              QFile::ExeGroup | QFile::ExeOther |
                                              QFile::ReadOwner | QFile::ReadGroup |
                                              QFile::ReadOther);
        LaunchAndQuit(downloaded);
        return;
    }
    // Windows 10 1803 and later ship bsdtar, which unpacks zip files.
    const QString tar =
        QDir{qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"))}.filePath(
            QStringLiteral("System32/tar.exe"));
    const QString root = WindowsInstallRoot();
    QProgressDialog unpacking{tr("Unpacking..."), QString(), 0, 0, parent_widget};
    unpacking.setWindowTitle(tr("Updating"));
    unpacking.setWindowModality(Qt::WindowModal);
    unpacking.setMinimumDuration(0);
    unpacking.show();
    QCoreApplication::processEvents();

    QProcess process;
    process.setProgram(tar);
    process.setArguments({QStringLiteral("-xf"), QDir::toNativeSeparators(downloaded),
                          QStringLiteral("-C"), QDir::toNativeSeparators(root)});
    process.start();
    const bool finished = process.waitForStarted(10000) && process.waitForFinished(300000);
    unpacking.close();
    const QString program = InstalledProgram();
    if (!finished || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        !QFileInfo::exists(program)) {
        LOG_WARNING(Frontend, "Unpacking {} into {} failed: {}", downloaded.toStdString(),
                    root.toStdString(), QString::fromLocal8Bit(process.readAllStandardError())
                                            .trimmed()
                                            .toStdString());
        OpenPage(tr("Couldn't unpack the update; the zip is at %1.")
                     .arg(QDir::toNativeSeparators(downloaded)));
        return;
    }
    QFile::remove(downloaded);
    LaunchAndQuit(program);
}

void TestbuildUpdater::LaunchAndQuit(const QString& program) {
    LOG_INFO(Frontend, "Starting {} and closing this version", program.toStdString());
    // Start it once this instance has closed and saved its settings.
    // Linux looks for a portable "user" folder in the working directory: keep ours. Windows looks
    // next to lichii.exe, so start it in its own folder.
    const QString working_directory =
        IsWindows ? QFileInfo{program}.absolutePath() : QDir::currentPath();
    connect(qApp, &QCoreApplication::aboutToQuit, qApp, [program, working_directory] {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        // This AppImage's runtime and AppRun settings would point the new one at our libraries.
        for (const char* name : {"APPIMAGE", "APPDIR", "ARGV0", "OWD", "LD_LIBRARY_PATH",
                                 "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                                 "QML2_IMPORT_PATH"}) {
            environment.remove(QString::fromLatin1(name));
        }
        QProcess process;
        process.setProgram(program);
        process.setWorkingDirectory(working_directory);
        process.setProcessEnvironment(environment);
        process.startDetached();
    });
    QTimer::singleShot(0, parent_widget, &QWidget::close);
}

void TestbuildUpdater::OpenPage(const QString& reason) {
    LOG_WARNING(Frontend, "Update: {}", reason.toStdString());
    QMessageBox::information(parent_widget, tr("Update"),
                             tr("%1\n\nThe release page opens in your browser instead.").arg(reason));
    QDesktopServices::openUrl(pending->page);
}
