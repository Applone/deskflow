/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <cstdint>
#include <string>
#include <vector>

namespace deskflow::platform {

class WaylandKeyboardManager : public QObject
{
  Q_OBJECT

public:
  static WaylandKeyboardManager &instance();

  int32_t getActiveGroup();
  std::string getActiveLanguage();
  bool getLayouts(std::string &layouts, std::string &variants);
  std::vector<std::string> getLayoutLanguageCodes();

public Q_SLOTS:
  void onKdeLayoutChanged(uint layout);
  void onKdeLayoutListChanged();

private:
  WaylandKeyboardManager();
  ~WaylandKeyboardManager() override = default;

  void initKde();
  void refreshLayouts();

  int32_t m_activeGroup = -1;
  std::string m_layouts;
  std::string m_variants;
  std::vector<std::string> m_langCodes;
  bool m_kdeConnected = false;
  bool m_isWayland = false;
};

} // namespace deskflow::platform
