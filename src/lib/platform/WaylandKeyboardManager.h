/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class QThread;
class QTimer;

namespace deskflow::platform {

/**
 * @brief Tracks the desktop environment's keyboard layout configuration on Wayland.
 *
 * All IPC happens on a private worker thread that runs a real Qt event loop.
 * That matters for two reasons:
 *
 *  - Qt delivers D-Bus signals by posting to the receiver thread's event queue.
 *    The core thread never runs a Qt event loop (App::run() blocks it in
 *    appUtil().run()), so a manager living there can never receive
 *    layoutChanged and has to fall back to polling.
 *  - The core thread also drives screen switching and key forwarding. Any
 *    synchronous D-Bus call or helper process on that thread stalls input, and
 *    a wedged or absent desktop service turns that into seconds of latency.
 *
 * The accessors below are therefore the *only* thing the core thread touches,
 * and they never perform IPC: they read a snapshot kept current by the worker.
 */
class WaylandKeyboardManager : public QObject
{
  Q_OBJECT

public:
  static WaylandKeyboardManager &instance();

  /// Active layout group. Cheap: reads a cached atomic, never does IPC.
  int32_t getActiveGroup() const;
  /// Language code of the active layout. Cheap: reads the cached snapshot.
  std::string getActiveLanguage() const;
  /// Configured layout and variant lists. Cheap: reads the cached snapshot.
  bool getLayouts(std::string &layouts, std::string &variants) const;
  /// Language codes for every configured layout. Cheap: reads the snapshot.
  std::vector<std::string> getLayoutLanguageCodes() const;

  /// Stop the worker thread. Registered as a Qt post routine; safe to call twice.
  void shutdown();

private Q_SLOTS:
  /// Runs on the worker thread once its event loop is up.
  void onWorkerStarted();
  void onKdeLayoutChanged(uint layout);
  void onKdeLayoutListChanged();
  /// Safety net for desktops that push no layout signals (e.g. GNOME).
  void onPollTimer();

private:
  WaylandKeyboardManager();
  ~WaylandKeyboardManager() override;

  /// One bounded, synchronous probe on the calling thread. Needed because the
  /// first keymap is built before the worker has had a chance to run.
  void bootstrapBlocking();

  void subscribeKde();
  void refreshActiveGroup();
  void refreshLayouts();

  bool queryKdeActiveGroup(int32_t &group);
  bool queryKdeLayoutList(std::string &layouts, std::string &variants);
  static bool queryKxkbrcLayoutList(std::string &layouts, std::string &variants);
  static bool queryGnomeLayoutList(std::string &layouts, std::string &variants);
  bool queryGnomeActiveGroup(int32_t &group);

  void publishLayouts(bool found, const std::string &layouts, const std::string &variants);

  /// Cap on any synchronous session-bus call. Only ever paid on the worker
  /// thread (or once during bootstrap), never on the input path.
  static constexpr int kDBusTimeoutMs = 300;
  /// Cap on the gsettings helper process.
  static constexpr int kProcessTimeoutMs = 400;
  /// Safety-net poll interval. Only used when the desktop pushes no signals.
  static constexpr int kPollIntervalMs = 2000;

  std::atomic<int32_t> m_activeGroup{0};
  std::atomic<bool> m_isWayland{false};
  /// True once a KDE query has succeeded. When true the layoutChanged signal is
  /// authoritative and the safety-net poll stays idle.
  std::atomic<bool> m_kdeAvailable{false};
  std::atomic<bool> m_gnomeAvailable{true};
  std::atomic<bool> m_stopped{false};

  mutable std::mutex m_mutex;
  std::string m_layouts;
  std::string m_variants;
  std::vector<std::string> m_langCodes;

  QThread *m_thread = nullptr;
  QTimer *m_pollTimer = nullptr;
};

} // namespace deskflow::platform
