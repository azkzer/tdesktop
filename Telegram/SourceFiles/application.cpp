/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "application.h"

#include "platform/platform_specific.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "storage/localstorage.h"
#include "window/notifications_manager.h"
#include "core/crash_reports.h"
#include "messenger.h"
#include "base/timer.h"
#include "core/update_checker.h"
#include "core/crash_report_window.h"
#include "lang/lang_keys.h"

namespace {

QChar _toHex(ushort v) {
	v = v & 0x000F;
	return QChar::fromLatin1((v >= 10) ? ('a' + (v - 10)) : ('0' + v));
}
ushort _fromHex(QChar c) {
	return ((c.unicode() >= uchar('a')) ? (c.unicode() - uchar('a') + 10) : (c.unicode() - uchar('0'))) & 0x000F;
}

QString _escapeTo7bit(const QString &str) {
	QString result;
	result.reserve(str.size() * 2);
	for (int i = 0, l = str.size(); i != l; ++i) {
		QChar ch(str.at(i));
		ushort uch(ch.unicode());
		if (uch < 32 || uch > 127 || uch == ushort(uchar('%'))) {
			result.append('%').append(_toHex(uch >> 12)).append(_toHex(uch >> 8)).append(_toHex(uch >> 4)).append(_toHex(uch));
		} else {
			result.append(ch);
		}
	}
	return result;
}

QString _escapeFrom7bit(const QString &str) {
	QString result;
	result.reserve(str.size());
	for (int i = 0, l = str.size(); i != l; ++i) {
		QChar ch(str.at(i));
		if (ch == QChar::fromLatin1('%') && i + 4 < l) {
			result.append(QChar(ushort((_fromHex(str.at(i + 1)) << 12) | (_fromHex(str.at(i + 2)) << 8) | (_fromHex(str.at(i + 3)) << 4) | _fromHex(str.at(i + 4)))));
			i += 4;
		} else {
			result.append(ch);
		}
	}
	return result;
}

} // namespace

Application::Application(
		not_null<Core::Launcher*> launcher,
		int &argc,
		char **argv)
: QApplication(argc, argv)
, _launcher(launcher)
#ifndef TDESKTOP_DISABLE_AUTOUPDATE
, _updateChecker(std::make_unique<Core::UpdateChecker>())
#endif // TDESKTOP_DISABLE_AUTOUPDATE
{
	const auto d = QFile::encodeName(QDir(cWorkingDir()).absolutePath());
	char h[33] = { 0 };
	hashMd5Hex(d.constData(), d.size(), h);
#ifndef OS_MAC_STORE
	_localServerName = psServerPrefix() + h + '-' + cGUIDStr();
#else // OS_MAC_STORE
	h[4] = 0; // use only first 4 chars
	_localServerName = psServerPrefix() + h;
#endif // OS_MAC_STORE

	connect(&_localSocket, SIGNAL(connected()), this, SLOT(socketConnected()));
	connect(&_localSocket, SIGNAL(disconnected()), this, SLOT(socketDisconnected()));
	connect(&_localSocket, SIGNAL(error(QLocalSocket::LocalSocketError)), this, SLOT(socketError(QLocalSocket::LocalSocketError)));
	connect(&_localSocket, SIGNAL(bytesWritten(qint64)), this, SLOT(socketWritten(qint64)));
	connect(&_localSocket, SIGNAL(readyRead()), this, SLOT(socketReading()));
	connect(&_localServer, SIGNAL(newConnection()), this, SLOT(newInstanceConnected()));

	QTimer::singleShot(0, this, SLOT(startApplication()));
	connect(this, SIGNAL(aboutToQuit()), this, SLOT(closeApplication()));

	if (cManyInstance()) {
		LOG(("Many instance allowed, starting..."));
		singleInstanceChecked();
	} else {
        LOG(("Connecting local socket to %1...").arg(_localServerName));
		_localSocket.connectToServer(_localServerName);
	}
}

Application::~Application() = default;

bool Application::event(QEvent *e) {
	if (e->type() == QEvent::Close) {
		App::quit();
	}
	return QApplication::event(e);
}

void Application::socketConnected() {
	LOG(("Socket connected, this is not the first application instance, sending show command..."));
	_secondInstance = true;

	QString commands;
	const QStringList &lst(cSendPaths());
	for (QStringList::const_iterator i = lst.cbegin(), e = lst.cend(); i != e; ++i) {
		commands += qsl("SEND:") + _escapeTo7bit(*i) + ';';
	}
	if (!cStartUrl().isEmpty()) {
		commands += qsl("OPEN:") + _escapeTo7bit(cStartUrl()) + ';';
	}
	commands += qsl("CMD:show;");

	DEBUG_LOG(("Application Info: writing commands %1").arg(commands));
	_localSocket.write(commands.toLatin1());
}

void Application::socketWritten(qint64/* bytes*/) {
	if (_localSocket.state() != QLocalSocket::ConnectedState) {
		LOG(("Socket is not connected %1").arg(_localSocket.state()));
		return;
	}
	if (_localSocket.bytesToWrite()) {
		return;
	}
	LOG(("Show command written, waiting response..."));
}

void Application::socketReading() {
	if (_localSocket.state() != QLocalSocket::ConnectedState) {
		LOG(("Socket is not connected %1").arg(_localSocket.state()));
		return;
	}
	_localSocketReadData.append(_localSocket.readAll());
	if (QRegularExpression("RES:(\\d+);").match(_localSocketReadData).hasMatch()) {
		uint64 pid = _localSocketReadData.mid(4, _localSocketReadData.length() - 5).toULongLong();
		psActivateProcess(pid);
		LOG(("Show command response received, pid = %1, activating and quitting...").arg(pid));
		return App::quit();
	}
}

void Application::socketError(QLocalSocket::LocalSocketError e) {
	if (App::quitting()) return;

	if (_secondInstance) {
		LOG(("Could not write show command, error %1, quitting...").arg(e));
		return App::quit();
	}

	if (e == QLocalSocket::ServerNotFoundError) {
		LOG(("This is the only instance of Telegreat, starting server and app..."));
	} else {
		LOG(("Socket connect error %1, starting server and app...").arg(e));
	}
	_localSocket.close();

// Local server does not work in WinRT build.
#ifndef Q_OS_WINRT
	psCheckLocalSocket(_localServerName);

	if (!_localServer.listen(_localServerName)) {
		LOG(("Failed to start listening to %1 server, error %2").arg(_localServerName).arg(int(_localServer.serverError())));
		return App::quit();
	}
#endif // !Q_OS_WINRT

#ifndef TDESKTOP_DISABLE_AUTOUPDATE
	if (!cNoStartUpdate() && Core::checkReadyUpdate()) {
		cSetRestartingUpdate(true);
		DEBUG_LOG(("Application Info: installing update instead of starting app..."));
		return App::quit();
	}
#endif // !TDESKTOP_DISABLE_AUTOUPDATE

	singleInstanceChecked();
}

void Application::singleInstanceChecked() {
	if (cManyInstance()) {
		Logs::multipleInstances();
	}

	Sandbox::start();
	refreshGlobalProxy();

	if (!Logs::started() || (!cManyInstance() && !Logs::instanceChecked())) {
		new NotStartedWindow();
	} else {
		const auto status = CrashReports::Start();
		if (status == CrashReports::CantOpen) {
			new NotStartedWindow();
		} else if (status == CrashReports::LastCrashed) {
			if (Sandbox::LastCrashDump().isEmpty()) { // don't handle bad closing for now
				if (CrashReports::Restart() == CrashReports::CantOpen) {
					new NotStartedWindow();
				} else {
					Sandbox::launch();
				}
			} else {
				new LastCrashedWindow();
			}
		} else {
			Sandbox::launch();
		}
	}
}

void Application::socketDisconnected() {
	if (_secondInstance) {
		DEBUG_LOG(("Application Error: socket disconnected before command response received, quitting..."));
		return App::quit();
	}
}

void Application::newInstanceConnected() {
	DEBUG_LOG(("Application Info: new local socket connected"));
	for (QLocalSocket *client = _localServer.nextPendingConnection(); client; client = _localServer.nextPendingConnection()) {
		_localClients.push_back(LocalClient(client, QByteArray()));
		connect(client, SIGNAL(readyRead()), this, SLOT(readClients()));
		connect(client, SIGNAL(disconnected()), this, SLOT(removeClients()));
	}
}

void Application::readClients() {
	// This method can be called before Messenger is constructed.
	QString startUrl;
	QStringList toSend;
	for (LocalClients::iterator i = _localClients.begin(), e = _localClients.end(); i != e; ++i) {
		i->second.append(i->first->readAll());
		if (i->second.size()) {
			QString cmds(QString::fromLatin1(i->second));
			int32 from = 0, l = cmds.length();
			for (int32 to = cmds.indexOf(QChar(';'), from); to >= from; to = (from < l) ? cmds.indexOf(QChar(';'), from) : -1) {
				QStringRef cmd(&cmds, from, to - from);
				if (cmd.startsWith(qsl("CMD:"))) {
					Sandbox::execExternal(cmds.mid(from + 4, to - from - 4));
					QByteArray response(qsl("RES:%1;").arg(QCoreApplication::applicationPid()).toLatin1());
					i->first->write(response.data(), response.size());
				} else if (cmd.startsWith(qsl("SEND:"))) {
					if (cSendPaths().isEmpty()) {
						toSend.append(_escapeFrom7bit(cmds.mid(from + 5, to - from - 5)));
					}
				} else if (cmd.startsWith(qsl("OPEN:"))) {
					if (cStartUrl().isEmpty()) {
						startUrl = _escapeFrom7bit(cmds.mid(from + 5, to - from - 5)).mid(0, 8192);
					}
				} else {
					LOG(("Application Error: unknown command %1 passed in local socket").arg(QString(cmd.constData(), cmd.length())));
				}
				from = to + 1;
			}
			if (from > 0) {
				i->second = i->second.mid(from);
			}
		}
	}
	if (!toSend.isEmpty()) {
		QStringList paths(cSendPaths());
		paths.append(toSend);
		cSetSendPaths(paths);
	}
	if (!cSendPaths().isEmpty()) {
		if (App::wnd()) {
			App::wnd()->sendPaths();
		}
	}
	if (!startUrl.isEmpty()) {
		cSetStartUrl(startUrl);
	}
	if (auto messenger = Messenger::InstancePointer()) {
		messenger->checkStartUrl();
	}
}

void Application::removeClients() {
	DEBUG_LOG(("Application Info: remove clients slot called, clients %1").arg(_localClients.size()));
	for (LocalClients::iterator i = _localClients.begin(), e = _localClients.end(); i != e;) {
		if (i->first->state() != QLocalSocket::ConnectedState) {
			DEBUG_LOG(("Application Info: removing client"));
			i = _localClients.erase(i);
			e = _localClients.end();
		} else {
			++i;
		}
	}
}

void Application::startApplication() {
	if (App::quitting()) {
		quit();
	}
}

void Application::createMessenger() {
	Expects(!App::quitting());

	_messengerInstance = std::make_unique<Messenger>(_launcher);
}

void Application::refreshGlobalProxy() {
#ifndef TDESKTOP_DISABLE_NETWORK_PROXY
	const auto proxy = [&] {
		if (Global::started()) {
			return Global::UseProxy()
				? Global::SelectedProxy()
				: ProxyData();
		}
		return Sandbox::PreLaunchProxy();
	}();
	if (proxy.type == ProxyData::Type::Socks5
		|| proxy.type == ProxyData::Type::Http) {
		QNetworkProxy::setApplicationProxy(ToNetworkProxy(proxy));
	} else {
		QNetworkProxyFactory::setUseSystemConfiguration(true);
	}
#endif // TDESKTOP_DISABLE_NETWORK_PROXY
}

void Application::closeApplication() {
	if (App::launchState() == App::QuitProcessed) return;
	App::setLaunchState(App::QuitProcessed);

	_messengerInstance.reset();

	Sandbox::finish();

	_localServer.close();
	for (LocalClients::iterator i = _localClients.begin(), e = _localClients.end(); i != e; ++i) {
		disconnect(i->first, SIGNAL(disconnected()), this, SLOT(removeClients()));
		i->first->close();
	}
	_localClients.clear();

	_localSocket.close();

#ifndef TDESKTOP_DISABLE_AUTOUPDATE
	_updateChecker = nullptr;
#endif // !TDESKTOP_DISABLE_AUTOUPDATE
}

inline Application *application() {
	return qobject_cast<Application*>(QApplication::instance());
}

namespace Sandbox {

QRect availableGeometry() {
	if (auto a = application()) {
		return a->desktop()->availableGeometry();
	}
	return QDesktopWidget().availableGeometry();
}

QRect screenGeometry(const QPoint &p) {
	if (auto a = application()) {
		return a->desktop()->screenGeometry(p);
	}
	return QDesktopWidget().screenGeometry(p);
}

void setActiveWindow(QWidget *window) {
	if (auto a = application()) {
		a->setActiveWindow(window);
	}
}

bool isSavingSession() {
	if (auto a = application()) {
		return a->isSavingSession();
	}
	return false;
}

void execExternal(const QString &cmd) {
	DEBUG_LOG(("Application Info: executing external command '%1'").arg(cmd));
	if (cmd == "show") {
		if (App::wnd()) {
			App::wnd()->activate();
		} else if (PreLaunchWindow::instance()) {
			PreLaunchWindow::instance()->activate();
		}
	}
}

void adjustSingleTimers() {
	if (auto a = application()) {
		a->adjustSingleTimers();
	}
	base::Timer::Adjust();
}

void connect(const char *signal, QObject *object, const char *method) {
	if (auto a = application()) {
		a->connect(a, signal, object, method);
	}
}

void launch() {
	Assert(application() != 0);

	float64 dpi = Application::primaryScreen()->logicalDotsPerInch();
	if (dpi <= 108) { // 0-96-108
		cSetScreenScale(dbisOne);
	} else if (dpi <= 132) { // 108-120-132
		cSetScreenScale(dbisOneAndQuarter);
	} else if (dpi <= 168) { // 132-144-168
		cSetScreenScale(dbisOneAndHalf);
	} else { // 168-192-inf
		cSetScreenScale(dbisTwo);
	}

	auto devicePixelRatio = application()->devicePixelRatio();
	if (devicePixelRatio > 1.) {
		if ((cPlatform() != dbipMac && cPlatform() != dbipMacOld) || (devicePixelRatio != 2.)) {
			LOG(("Found non-trivial Device Pixel Ratio: %1").arg(devicePixelRatio));
			LOG(("Environmental variables: QT_DEVICE_PIXEL_RATIO='%1'").arg(QString::fromLatin1(qgetenv("QT_DEVICE_PIXEL_RATIO"))));
			LOG(("Environmental variables: QT_SCALE_FACTOR='%1'").arg(QString::fromLatin1(qgetenv("QT_SCALE_FACTOR"))));
			LOG(("Environmental variables: QT_AUTO_SCREEN_SCALE_FACTOR='%1'").arg(QString::fromLatin1(qgetenv("QT_AUTO_SCREEN_SCALE_FACTOR"))));
			LOG(("Environmental variables: QT_SCREEN_SCALE_FACTORS='%1'").arg(QString::fromLatin1(qgetenv("QT_SCREEN_SCALE_FACTORS"))));
		}
		cSetRetina(true);
		cSetRetinaFactor(devicePixelRatio);
		cSetIntRetinaFactor(int32(cRetinaFactor()));
		cSetConfigScale(dbisOne);
		cSetRealScale(dbisOne);
	}

	application()->createMessenger();
}

void refreshGlobalProxy() {
	if (const auto instance = application()) {
		instance->refreshGlobalProxy();
	}
}

} // namespace Sandbox
