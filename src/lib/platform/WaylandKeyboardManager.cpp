/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WaylandKeyboardManager.h"
#include "base/Log.h"
#include "common/PlatformInfo.h"

#include <sstream>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>

#if WINAPI_XWINDOWS
#include "deskflow/unix/XkbLayoutsParser.h"
#endif

namespace deskflow::platform {

WaylandKeyboardManager &WaylandKeyboardManager::instance()
{
  static WaylandKeyboardManager s_instance;
  return s_instance;
}

WaylandKeyboardManager::WaylandKeyboardManager()
{
  m_isWayland = deskflow::platform::isWayland();
  if (!m_isWayland) {
    return;
  }

  initKde();
  refreshLayouts();
}

void WaylandKeyboardManager::initKde()
{
  if (!QDBusConnection::sessionBus().isConnected()) {
    return;
  }

  QDBusInterface kbd("org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", QDBusConnection::sessionBus());
  if (kbd.isValid()) {
    m_kdeConnected = true;
    QDBusConnection::sessionBus().connect(
        "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "layoutChanged", this,
        SLOT(onKdeLayoutChanged(uint))
    );
    QDBusConnection::sessionBus().connect(
        "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "layoutListChanged", this,
        SLOT(onKdeLayoutListChanged())
    );

    QDBusReply<uint> reply = kbd.call("getLayout");
    if (reply.isValid()) {
      m_activeGroup = static_cast<int32_t>(reply.value());
      LOG_DEBUG("Wayland KDE active layout group: %d", m_activeGroup);
    }
  }
}

void WaylandKeyboardManager::onKdeLayoutChanged(uint layout)
{
  m_activeGroup = static_cast<int32_t>(layout);
  LOG_DEBUG("Wayland KDE layoutChanged signal received: %d", m_activeGroup);
}

void WaylandKeyboardManager::onKdeLayoutListChanged()
{
  LOG_DEBUG("Wayland KDE layoutListChanged signal received");
  refreshLayouts();
}

void WaylandKeyboardManager::refreshLayouts()
{
  m_layouts.clear();
  m_variants.clear();
  m_langCodes.clear();

  // 1. Try KDE DBus
  if (QDBusConnection::sessionBus().isConnected()) {
    QDBusInterface kbd("org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", QDBusConnection::sessionBus());
    if (kbd.isValid()) {
      QDBusMessage reply = kbd.call("getLayoutsList");
      if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::ArrayType) {
          arg.beginArray();
          std::vector<std::string> lList;
          std::vector<std::string> vList;
          while (!arg.atEnd()) {
            arg.beginStructure();
            QString shortName, displayName, longName;
            arg >> shortName >> displayName >> longName;
            arg.endStructure();
            if (!shortName.isEmpty()) {
              lList.push_back(shortName.toStdString());
              vList.push_back("");
            }
          }
          arg.endArray();
          if (!lList.empty()) {
            for (size_t i = 0; i < lList.size(); ++i) {
              if (i > 0) m_layouts += ",";
              m_layouts += lList[i];
            }
            for (size_t i = 0; i < vList.size(); ++i) {
              if (i > 0) m_variants += ",";
              m_variants += vList[i];
            }
          }
        }
      }
    }
  }

  // 2. Try KDE kxkbrc config file if layouts still empty
  if (m_layouts.empty()) {
    QString kxkbrcPath = QDir::homePath() + "/.config/kxkbrc";
    if (QFile::exists(kxkbrcPath)) {
      QSettings kxkbrc(kxkbrcPath, QSettings::IniFormat);
      QString l = kxkbrc.value("Layout/LayoutList").toString();
      QString v = kxkbrc.value("Layout/VariantList").toString();
      if (!l.isEmpty()) {
        m_layouts = l.toStdString();
        m_variants = v.toStdString();
      }
    }
  }

  // 3. Try GNOME via gsettings
  if (m_layouts.empty()) {
    QProcess proc;
    proc.start("gsettings", {"get", "org.gnome.desktop.input-sources", "sources"});
    if (proc.waitForFinished(1000) && proc.exitCode() == 0) {
      QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
      if (out.startsWith("[") && out.endsWith("]")) {
        QStringList lList;
        QStringList vList;
        int idx = 0;
        while ((idx = out.indexOf("('xkb', '", idx)) != -1) {
          idx += 9;
          int end = out.indexOf("')", idx);
          if (end != -1) {
            QString src = out.mid(idx, end - idx);
            int plus = src.indexOf('+');
            if (plus != -1) {
              lList.append(src.left(plus));
              vList.append(src.mid(plus + 1));
            } else {
              lList.append(src);
              vList.append("");
            }
            idx = end + 2;
          }
        }
        if (!lList.isEmpty()) {
          m_layouts = lList.join(",").toStdString();
          m_variants = vList.join(",").toStdString();
        }
      }
    }
  }

  // 4. Populate language codes from layouts
#if WINAPI_XWINDOWS
  if (!m_layouts.empty()) {
    std::stringstream ls(m_layouts);
    std::string item;
    while (std::getline(ls, item, ',')) {
      std::string iso = XkbLayoutsParser::convertLayoutToISO(item);
      if (!iso.empty()) {
        m_langCodes.push_back(iso);
      } else {
        m_langCodes.push_back(item);
      }
    }
  }
#endif

  LOG_DEBUG("Wayland detected layouts: \"%s\", langCodes: %zu", m_layouts.c_str(), m_langCodes.size());
}

bool WaylandKeyboardManager::getLayouts(std::string &layouts, std::string &variants)
{
  if (!m_isWayland) {
    return false;
  }
  if (m_layouts.empty()) {
    refreshLayouts();
  }
  layouts = m_layouts;
  variants = m_variants;
  return !m_layouts.empty();
}

std::vector<std::string> WaylandKeyboardManager::getLayoutLanguageCodes()
{
  if (m_langCodes.empty()) {
    refreshLayouts();
  }
  return m_langCodes;
}

int32_t WaylandKeyboardManager::getActiveGroup()
{
  if (!m_isWayland) {
    return 0;
  }

  // On KDE, the layout is tracked via the layoutChanged signal so the cached
  // m_activeGroup is always current.  Avoid a synchronous D-Bus call on every
  // key event — the repeated introspection + method calls flood the session
  // bus and delay the portal "Input Capture started" notification.
  if (m_kdeConnected) {
    return m_activeGroup >= 0 ? m_activeGroup : 0;
  }

  // On GNOME, query gsettings mru-sources
  QProcess proc;
  proc.start("gsettings", {"get", "org.gnome.desktop.input-sources", "mru-sources"});
  if (proc.waitForFinished(500) && proc.exitCode() == 0) {
    QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    int idx = out.indexOf("('xkb', '");
    if (idx != -1) {
      idx += 9;
      int end = out.indexOf("')", idx);
      if (end != -1) {
        QString currentSrc = out.mid(idx, end - idx);
        int plus = currentSrc.indexOf('+');
        if (plus != -1) currentSrc = currentSrc.left(plus);
        // Find index in m_layouts
        std::stringstream ls(m_layouts);
        std::string item;
        int32_t groupIndex = 0;
        while (std::getline(ls, item, ',')) {
          if (item == currentSrc.toStdString()) {
            m_activeGroup = groupIndex;
            return m_activeGroup;
          }
          groupIndex++;
        }
      }
    }
  }

  return m_activeGroup >= 0 ? m_activeGroup : 0;
}

std::string WaylandKeyboardManager::getActiveLanguage()
{
  if (!m_isWayland) {
    return "";
  }

  int32_t group = getActiveGroup();
  if (m_langCodes.empty()) {
    refreshLayouts();
  }

  if (group >= 0 && static_cast<size_t>(group) < m_langCodes.size()) {
    return m_langCodes[group];
  }

  return "";
}

} // namespace deskflow::platform
