/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WaylandKeyboardManager.h"
#include "base/Log.h"
#include "common/PlatformInfo.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <sstream>

#if WINAPI_XWINDOWS
#include "deskflow/unix/XkbLayoutsParser.h"
#endif

namespace deskflow::platform {

namespace {

constexpr auto kKdeService = "org.kde.keyboard";
constexpr auto kKdePath = "/Layouts";
constexpr auto kKdeInterface = "org.kde.KeyboardLayouts";

std::string joinCsv(const QStringList &values)
{
  return values.join(QStringLiteral(",")).toStdString();
}

std::vector<std::string> splitCsv(const std::string &value)
{
  std::vector<std::string> out;
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, ',')) {
    out.push_back(item);
  }
  return out;
}

/// Parse a gsettings list of (type, id) pairs, e.g.
/// [('xkb', 'us'), ('xkb', 'ru+phonetic')], into layout and variant lists.
bool parseGnomeSources(const QString &text, QStringList &layouts, QStringList &variants)
{
  static const QString prefix = QStringLiteral("('xkb', '");
  int index = 0;
  while ((index = text.indexOf(prefix, index)) != -1) {
    index += prefix.size();
    const int end = text.indexOf(QStringLiteral("')"), index);
    if (end == -1) {
      break;
    }
    const QString source = text.mid(index, end - index);
    const int plus = source.indexOf(QLatin1Char('+'));
    if (plus != -1) {
      layouts.append(source.left(plus));
      variants.append(source.mid(plus + 1));
    } else {
      layouts.append(source);
      variants.append(QString());
    }
    index = end + 2;
  }
  return !layouts.isEmpty();
}

} // namespace

WaylandKeyboardManager &WaylandKeyboardManager::instance()
{
  // Deliberately leaked rather than a static local with a destructor: tearing
  // a QThread down from static-destruction order, after QCoreApplication is
  // already gone, is a reliable way to hang or crash on exit. shutdown() is
  // registered as a Qt post routine instead, which runs while Qt is still
  // alive.
  static WaylandKeyboardManager *s_instance = [] {
    auto *manager = new WaylandKeyboardManager();
    qAddPostRoutine([] { WaylandKeyboardManager::instance().shutdown(); });
    return manager;
  }();
  return *s_instance;
}

WaylandKeyboardManager::WaylandKeyboardManager()
{
  m_isWayland = deskflow::platform::isWayland();
  if (!m_isWayland) {
    return;
  }

  // The first keymap is compiled before the worker thread can possibly have
  // run, so pay for one bounded probe here and never block a caller again.
  bootstrapBlocking();

  m_thread = new QThread();
  m_thread->setObjectName(QStringLiteral("deskflow-kbd-layout"));
  moveToThread(m_thread);
  connect(m_thread, &QThread::started, this, &WaylandKeyboardManager::onWorkerStarted);
  m_thread->start();
}

WaylandKeyboardManager::~WaylandKeyboardManager()
{
  shutdown();
}

void WaylandKeyboardManager::shutdown()
{
  if (m_stopped.exchange(true)) {
    return;
  }

  if (m_thread) {
    m_thread->quit();
    if (!m_thread->wait(2000)) {
      LOG_WARN("keyboard layout worker did not stop, terminating");
      m_thread->terminate();
      m_thread->wait(500);
    }
    // Not deleted: this object still reports m_thread as its thread affinity,
    // and the singleton itself is intentionally leaked. Freeing the QThread
    // here would leave that pointer dangling for no benefit at exit.
    m_thread = nullptr;
  }
}

void WaylandKeyboardManager::bootstrapBlocking()
{
  int32_t group = -1;
  if (queryKdeActiveGroup(group)) {
    m_activeGroup = group;
  }
  refreshLayouts();
}

void WaylandKeyboardManager::onWorkerStarted()
{
  LOG_DEBUG("keyboard layout worker started");

  subscribeKde();
  refreshActiveGroup();
  refreshLayouts();

  // Safety net only. On KDE the layoutChanged signal is delivered to this
  // thread's event loop and the timer short-circuits; on desktops that push
  // nothing (GNOME) this is what keeps the cache fresh - off the input path.
  m_pollTimer = new QTimer(this);
  m_pollTimer->setInterval(kPollIntervalMs);
  connect(m_pollTimer, &QTimer::timeout, this, &WaylandKeyboardManager::onPollTimer);
  m_pollTimer->start();
}

void WaylandKeyboardManager::subscribeKde()
{
  if (!QDBusConnection::sessionBus().isConnected()) {
    return;
  }

  // Now that we own an event loop these actually get delivered, so the active
  // group is push-updated and needs no polling on KDE.
  const bool changed = QDBusConnection::sessionBus().connect(
      kKdeService, kKdePath, kKdeInterface, QStringLiteral("layoutChanged"), this, SLOT(onKdeLayoutChanged(uint))
  );
  const bool listChanged = QDBusConnection::sessionBus().connect(
      kKdeService, kKdePath, kKdeInterface, QStringLiteral("layoutListChanged"), this, SLOT(onKdeLayoutListChanged())
  );

  LOG_DEBUG("kde layout signals subscribed: layoutChanged=%d layoutListChanged=%d", changed, listChanged);
}

void WaylandKeyboardManager::onKdeLayoutChanged(uint layout)
{
  m_activeGroup = static_cast<int32_t>(layout);
  m_kdeAvailable = true;
  LOG_DEBUG("kde layoutChanged: group=%d", m_activeGroup.load());
}

void WaylandKeyboardManager::onKdeLayoutListChanged()
{
  LOG_DEBUG("kde layoutListChanged");
  refreshLayouts();
  refreshActiveGroup();
}

void WaylandKeyboardManager::onPollTimer()
{
  // KDE pushes layoutChanged, so there is nothing to poll for. Re-probe only
  // occasionally in case the keyboard daemon was restarted and our
  // subscription outlived it.
  if (m_kdeAvailable) {
    static int ticks = 0;
    if (++ticks % 15 != 0) { // ~30s
      return;
    }
  }

  refreshActiveGroup();
}

void WaylandKeyboardManager::refreshActiveGroup()
{
  int32_t group = -1;

  if (queryKdeActiveGroup(group)) {
    m_activeGroup = group;
    return;
  }

  if (m_gnomeAvailable && queryGnomeActiveGroup(group)) {
    m_activeGroup = group;
    return;
  }

  // Nothing answered; keep the last known value rather than guessing 0 and
  // sending characters from the wrong layout.
}

bool WaylandKeyboardManager::queryKdeActiveGroup(int32_t &group)
{
  if (!QDBusConnection::sessionBus().isConnected()) {
    return false;
  }

  // Direct message rather than QDBusInterface: constructing one performs a
  // blocking Introspect round trip we never use, with D-Bus's 25s default
  // timeout.
  auto message = QDBusMessage::createMethodCall(kKdeService, kKdePath, kKdeInterface, QStringLiteral("getLayout"));
  const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, kDBusTimeoutMs);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
    m_kdeAvailable = false;
    return false;
  }

  bool ok = false;
  const auto value = reply.arguments().at(0).toUInt(&ok);
  if (!ok) {
    m_kdeAvailable = false;
    return false;
  }

  group = static_cast<int32_t>(value);
  m_kdeAvailable = true;
  return true;
}

bool WaylandKeyboardManager::queryKdeLayoutList(std::string &layouts, std::string &variants)
{
  if (!QDBusConnection::sessionBus().isConnected()) {
    return false;
  }

  auto message =
      QDBusMessage::createMethodCall(kKdeService, kKdePath, kKdeInterface, QStringLiteral("getLayoutsList"));
  const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, kDBusTimeoutMs);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
    return false;
  }

  const auto arg = reply.arguments().at(0).value<QDBusArgument>();
  if (arg.currentType() != QDBusArgument::ArrayType) {
    return false;
  }

  QStringList layoutList;
  QStringList variantList;
  arg.beginArray();
  while (!arg.atEnd()) {
    arg.beginStructure();
    QString shortName;
    QString displayName;
    QString longName;
    arg >> shortName >> displayName >> longName;
    arg.endStructure();
    if (shortName.isEmpty()) {
      continue;
    }
    // getLayoutsList carries no variant field, so a layout configured as
    // "us(dvorak)" would silently compile as plain "us". Recover it from the
    // long name; kxkbrc is preferred and tried first.
    QString variant;
    const int open = longName.indexOf(QLatin1Char('('));
    const int close = longName.lastIndexOf(QLatin1Char(')'));
    if (open != -1 && close > open) {
      variant = longName.mid(open + 1, close - open - 1);
    }
    layoutList.append(shortName);
    variantList.append(variant);
  }
  arg.endArray();

  if (layoutList.isEmpty()) {
    return false;
  }

  layouts = joinCsv(layoutList);
  variants = joinCsv(variantList);
  return true;
}

bool WaylandKeyboardManager::queryKxkbrcLayoutList(std::string &layouts, std::string &variants)
{
  const QString path = QDir::homePath() + QStringLiteral("/.config/kxkbrc");
  if (!QFile::exists(path)) {
    return false;
  }

  QSettings kxkbrc(path, QSettings::IniFormat);
  const QString layoutList = kxkbrc.value(QStringLiteral("Layout/LayoutList")).toString();
  if (layoutList.isEmpty()) {
    return false;
  }

  layouts = layoutList.toStdString();
  variants = kxkbrc.value(QStringLiteral("Layout/VariantList")).toString().toStdString();
  return true;
}

bool WaylandKeyboardManager::queryGnomeLayoutList(std::string &layouts, std::string &variants)
{
  QProcess proc;
  proc.start(
      QStringLiteral("gsettings"), {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.input-sources"),
                                    QStringLiteral("sources")}
  );
  if (!proc.waitForFinished(kProcessTimeoutMs) || proc.exitCode() != 0) {
    proc.kill();
    proc.waitForFinished(kProcessTimeoutMs);
    return false;
  }

  QStringList layoutList;
  QStringList variantList;
  if (!parseGnomeSources(QString::fromUtf8(proc.readAllStandardOutput()).trimmed(), layoutList, variantList)) {
    return false;
  }

  layouts = joinCsv(layoutList);
  variants = joinCsv(variantList);
  return true;
}

bool WaylandKeyboardManager::queryGnomeActiveGroup(int32_t &group)
{
  QProcess proc;
  proc.start(
      QStringLiteral("gsettings"), {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.input-sources"),
                                    QStringLiteral("mru-sources")}
  );
  if (!proc.waitForFinished(kProcessTimeoutMs) || proc.exitCode() != 0) {
    proc.kill();
    proc.waitForFinished(kProcessTimeoutMs);
    m_gnomeAvailable = false;
    return false;
  }

  QStringList layoutList;
  QStringList variantList;
  if (!parseGnomeSources(QString::fromUtf8(proc.readAllStandardOutput()).trimmed(), layoutList, variantList)) {
    m_gnomeAvailable = false;
    return false;
  }

  m_gnomeAvailable = true;

  // mru-sources is most-recently-used first, so element 0 is active.
  const std::string current = layoutList.first().toStdString();
  std::vector<std::string> known;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    known = splitCsv(m_layouts);
  }

  for (size_t i = 0; i < known.size(); ++i) {
    if (known[i] == current) {
      group = static_cast<int32_t>(i);
      return true;
    }
  }

  return false;
}

void WaylandKeyboardManager::refreshLayouts()
{
  std::string layouts;
  std::string variants;

  // kxkbrc first: unlike the KDE D-Bus list it carries variants verbatim.
  bool found = queryKxkbrcLayoutList(layouts, variants);
  if (!found) {
    found = queryKdeLayoutList(layouts, variants);
  }
  if (!found && m_gnomeAvailable) {
    found = queryGnomeLayoutList(layouts, variants);
    if (!found) {
      m_gnomeAvailable = false;
    }
  }

  publishLayouts(found, layouts, variants);
}

void WaylandKeyboardManager::publishLayouts(
    bool found, const std::string &layouts, const std::string &variants
)
{
  std::vector<std::string> langCodes;
#if WINAPI_XWINDOWS
  if (found) {
    for (const auto &layout : splitCsv(layouts)) {
      std::string iso = XkbLayoutsParser::convertLayoutToISO(layout);
      langCodes.push_back(iso.empty() ? layout : iso);
    }
  }
#endif

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Keep the previous snapshot if this probe found nothing: a transient
    // failure must not blank out a layout list that was working.
    if (!found && !m_layouts.empty()) {
      return;
    }
    m_layouts = found ? layouts : std::string();
    m_variants = found ? variants : std::string();
    m_langCodes = std::move(langCodes);
  }

  LOG_DEBUG("wayland layouts: \"%s\" variants: \"%s\"", layouts.c_str(), variants.c_str());
}

int32_t WaylandKeyboardManager::getActiveGroup() const
{
  if (!m_isWayland) {
    return 0;
  }

  const auto group = m_activeGroup.load();
  return group >= 0 ? group : 0;
}

bool WaylandKeyboardManager::getLayouts(std::string &layouts, std::string &variants) const
{
  if (!m_isWayland) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  layouts = m_layouts;
  variants = m_variants;
  return !m_layouts.empty();
}

std::vector<std::string> WaylandKeyboardManager::getLayoutLanguageCodes() const
{
  if (!m_isWayland) {
    return {};
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  return m_langCodes;
}

std::string WaylandKeyboardManager::getActiveLanguage() const
{
  if (!m_isWayland) {
    return "";
  }

  const auto group = getActiveGroup();

  std::lock_guard<std::mutex> lock(m_mutex);
  if (group >= 0 && static_cast<size_t>(group) < m_langCodes.size()) {
    return m_langCodes[static_cast<size_t>(group)];
  }

  return "";
}

} // namespace deskflow::platform
