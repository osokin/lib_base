/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "base/platform/linux/base_linux_wayland_integration.h"

#include "base/platform/linux/base_linux_wayland_utilities.h"
#include "base/platform/base_platform_info.h"
#include "base/qt_signal_producer.h"
#include "base/flat_map.h"

#include "qwayland-wayland.h"
#include "qwayland-xdg-foreign-unstable-v2.h"
#include "qwayland-idle-inhibit-unstable-v1.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <qpa/qplatformwindow_p.h>

using namespace QNativeInterface;
using namespace QNativeInterface::Private;
using namespace base::Platform::Wayland;

namespace base {
namespace Platform {
namespace {

class XdgExported : public AutoDestroyer<QtWayland::zxdg_exported_v2> {
public:
	XdgExported(
		struct ::wl_display *display,
		struct ::zxdg_exported_v2 *object)
	: AutoDestroyer(object) {
		wl_display_roundtrip(display);
	}

	[[nodiscard]] QString handle() const {
		return _handle;
	}

protected:
	void zxdg_exported_v2_handle(const QString &handle) override {
		_handle = handle;
	}

private:
	QString _handle;
};

} // namespace

struct WaylandIntegration::Private : public AutoDestroyer<QtWayland::wl_registry> {
	AutoDestroyer<QtWayland::zxdg_exporter_v2> xdgExporter;
	uint32_t xdgExporterName = 0;
	base::flat_map<wl_surface*, XdgExported> xdgExporteds;
	AutoDestroyer<QtWayland::zwp_idle_inhibit_manager_v1> idleInhibitManager;
	uint32_t idleInhibitManagerName = 0;
	base::flat_map<
		QWindow*,
		AutoDestroyer<QtWayland::zwp_idle_inhibitor_v1>
	> idleInhibitors;
	rpl::lifetime lifetime;

protected:
	void registry_global(
			uint32_t name,
			const QString &interface,
			uint32_t version) override {
		if (interface == qstr("zxdg_exporter_v2")) {
			xdgExporter.init(object(), name, version);
			xdgExporterName = name;
		} else if (interface == qstr("zwp_idle_inhibit_manager_v1")) {
			idleInhibitManager.init(object(), name, version);
			idleInhibitManagerName = name;
		}
	}

	void registry_global_remove(uint32_t name) override {
		if (name == xdgExporterName) {
			xdgExporter = {};
			xdgExporterName = 0;
		} else if (name == idleInhibitManagerName) {
			idleInhibitManager = {};
			idleInhibitManagerName = 0;
		}
	}
};

WaylandIntegration::WaylandIntegration()
: _private(std::make_unique<Private>()) {
	const auto native = qApp->nativeInterface<QWaylandApplication>();
	if (!native) {
		return;
	}

	const auto display = native->display();
	if (!display) {
		return;
	}

	_private->init(wl_display_get_registry(display));
	wl_display_roundtrip(display);
}

WaylandIntegration::~WaylandIntegration() = default;

WaylandIntegration *WaylandIntegration::Instance() {
	if (!::Platform::IsWayland()) return nullptr;
	static std::optional<WaylandIntegration> instance(std::in_place);
	base::qt_signal_producer(
		QGuiApplication::platformNativeInterface(),
		&QObject::destroyed
	) | rpl::start_with_next([&] {
		instance = std::nullopt;
	}, instance->_private->lifetime);
	if (!instance) return nullptr;
	return &*instance;
}

QString WaylandIntegration::nativeHandle(QWindow *window) {
	if (!_private->xdgExporter.isInitialized()) {
		return {};
	}

	const auto native = qApp->nativeInterface<QWaylandApplication>();
	const auto nativeWindow = window->nativeInterface<QWaylandWindow>();
	if (!native || !nativeWindow) {
		return {};
	}

	const auto display = native->display();
	const auto surface = nativeWindow->surface();

	if (!display || !surface) {
		return {};
	}

	const auto it = _private->xdgExporteds.find(surface);
	if (it != _private->xdgExporteds.cend()) {
		return it->second.handle();
	}

	const auto result = _private->xdgExporteds.emplace(
		surface,
		XdgExported(
			display,
			_private->xdgExporter.export_toplevel(surface)));

	base::qt_signal_producer(
		nativeWindow,
		&QWaylandWindow::surfaceDestroyed
	) | rpl::start_with_next([=] {
		auto it = _private->xdgExporteds.find(surface);
		if (it != _private->xdgExporteds.cend()) {
			_private->xdgExporteds.erase(it);
		}
	}, _private->lifetime);

	return result.first->second.handle();
}

QString WaylandIntegration::activationToken() {
	const auto window = QGuiApplication::focusWindow();
	if (!window) {
		return {};
	}

	const auto native = qApp->nativeInterface<QWaylandApplication>();
	const auto nativeWindow = window->nativeInterface<QWaylandWindow>();
	if (!native || !nativeWindow) {
		return {};
	}

	QEventLoop loop;
	QString token;

	base::qt_signal_producer(
		nativeWindow,
		&QWaylandWindow::xdgActivationTokenCreated
	) | rpl::start_with_next([&](const QString &tokenArg) {
		token = tokenArg;
		loop.quit();
	}, _private->lifetime);

	nativeWindow->requestXdgActivationToken(native->lastInputSerial());
	loop.exec();
	return token;
}

void WaylandIntegration::preventDisplaySleep(bool prevent, QWindow *window) {
	const auto deleter = [=] {
		auto it = _private->idleInhibitors.find(window);
		if (it != _private->idleInhibitors.cend()) {
			_private->idleInhibitors.erase(it);
		}
	};

	if (!prevent) {
		deleter();
		return;
	}

	if (_private->idleInhibitors.contains(window)) {
		return;
	}

	if (!_private->idleInhibitManager.isInitialized()) {
		return;
	}

	const auto native = window->nativeInterface<QWaylandWindow>();
	if (!native) {
		return;
	}

	const auto surface = native->surface();
	if (!surface) {
		return;
	}

	const auto inhibitor = _private->idleInhibitManager.create_inhibitor(
		surface);

	if (!inhibitor) {
		return;
	}

	_private->idleInhibitors.emplace(window, inhibitor);

	base::qt_signal_producer(
		native,
		&QWaylandWindow::surfaceDestroyed
	) | rpl::start_with_next(deleter, _private->lifetime);
}

} // namespace Platform
} // namespace base
