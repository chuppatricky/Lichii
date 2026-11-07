// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>

#include <QObject>
#include <QString>
#include <QUrl>

#include "common/common_types.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
class QSaveFile;
class QWidget;

/// Checks chuppatricky/Lichii's GitHub releases at startup for a newer chuppatricky-amd-testbuild
/// and asks whether to update or stay on this version. Updating downloads the new build next to
/// this one (Windows: the zip, unpacked into its own folder; Linux: the AppImage), starts it and
/// closes this one; settings, saves and shaders are shared. If that can't be done, it opens the
/// release page instead.
class TestbuildUpdater : public QObject {
    Q_OBJECT

public:
    /// is_busy: whether a game is running; the question then waits for ShowPending().
    explicit TestbuildUpdater(QWidget* parent, std::function<bool()> is_busy);
    ~TestbuildUpdater() override;

    /// Starts the check (once per launch).
    void Check();

    /// Asks about an update found while a game was running.
    void ShowPending();

private:
    struct Update {
        u32 version{};
        QString tag;
        QUrl page;
        QString notes;
        QString asset_name;
        QUrl asset_url;
        qint64 asset_size{};
    };

    void OnReleases(QNetworkReply* reply);
    void Ask();
    void StartDownload();
    void OnDownloadFinished();
    void Install(const QString& downloaded);
    void LaunchAndQuit(const QString& program);
    void OpenPage(const QString& reason);

    [[nodiscard]] QString DownloadPath() const;
    [[nodiscard]] QString InstalledProgram() const;

    QWidget* parent_widget;
    std::function<bool()> is_busy;
    QNetworkAccessManager* network;
    u32 current_version{};
    std::optional<Update> pending;
    bool asked{};

    QNetworkReply* download{};
    QSaveFile* download_file{};
    QProgressDialog* progress{};
};
