// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/rhi/rhi_surface.h"

#include "ui/rhi/rhi_renderer.h"
#include "ui/rp_widget.h"
#include "ui/painter.h"
#include "base/debug_log.h"
#include "base/platform/base_platform_info.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <QRhiWidget>
#include <QBackingStore>
#include <rhi/qrhi.h>
#include <QtGui/QWindow>
#include <qpa/qplatformbackingstore.h>
#endif // Qt >= 6.7

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <commctrl.h>
#endif // Q_OS_WIN

namespace Ui::GL {

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
namespace {

struct SurfaceRhiTraits : RpWidgetDefaultTraits {
	static constexpr bool kSetZeroGeometry = false;
};

} // namespace

class SurfaceRhi final
	: public RpWidgetBase<QRhiWidget, SurfaceRhiTraits> {
public:
	SurfaceRhi(QWidget *parent, std::unique_ptr<Renderer> renderer);
	~SurfaceRhi();

protected:
	void initialize(QRhiCommandBuffer *cb) override;
	void render(QRhiCommandBuffer *cb) override;
	void releaseResources() override;
	bool eventHook(QEvent *e) override;

private:
	[[nodiscard]] Rhi::Renderer *rhiRenderer() const;
	void ensureBackingStoreRhi();
#ifdef Q_OS_WIN
	void installExStyleFilterWin();
	void removeExStyleFilterWin();
#endif // Q_OS_WIN

	const std::unique_ptr<Renderer> _renderer;
	bool _backingStoreConfigured = false;
#ifdef Q_OS_WIN
	HWND _exStyleFilterHwnd = nullptr;
#endif // Q_OS_WIN

};

SurfaceRhi::SurfaceRhi(
	QWidget *parent,
	std::unique_ptr<Renderer> renderer)
: RpWidgetBase<QRhiWidget, SurfaceRhiTraits>(parent)
, _renderer(std::move(renderer)) {
#ifdef Q_OS_MAC
	setApi(::Platform::MetalSupported()
		? QRhiWidget::Api::Metal
		: QRhiWidget::Api::OpenGL);
#elif defined(Q_OS_WIN)
	// Follow the main-window QRhi backend decision made at startup in
	// Platform::SetupQtRhi and exposed via QT_WIDGETS_RHI_BACKEND so
	// the overlay uses the same backend as the rest of the app.
	//  * "d3d11" — used on Win8+ by default, and on Win7 as a fallback
	//    when desktop GL is blacklisted for the current GPU.
	//  * "opengl" — used on Win7 with a healthy GL driver.
	//  * unset (main window on Qt raster) — attempt GL anyway;
	//    QRhiWidget has an internal software fallback that degrades
	//    gracefully when the driver refuses context creation.
	const auto backend = qgetenv("QT_WIDGETS_RHI_BACKEND");
	setApi((backend == "d3d11")
		? QRhiWidget::Api::Direct3D11
		: QRhiWidget::Api::OpenGL);
#else
	setApi(QRhiWidget::Api::OpenGL);
#endif
	LOG(("QRhi: SurfaceRhi created"));
}

SurfaceRhi::~SurfaceRhi() {
	// Call releaseResources() here in the destructor body, BEFORE
	// member destruction begins. At this point QRhiWidget's QRhi is
	// still alive (base destructor hasn't run yet), so QRhi resource
	// deletion is safe. This handles the deleteChildren() teardown
	// path where Qt doesn't call releaseResources() automatically.
	releaseResources();
#ifdef Q_OS_WIN
	removeExStyleFilterWin();
#endif // Q_OS_WIN
}

#ifdef Q_OS_WIN
namespace {

constexpr UINT_PTR kExStyleSubclassId = 0x51F4C4FA;

LRESULT CALLBACK StripLayeredExStyleSubclass(
		HWND hwnd,
		UINT msg,
		WPARAM wParam,
		LPARAM lParam,
		UINT_PTR uIdSubclass,
		DWORD_PTR /*dwRefData*/) {
	// WS_EX_LAYERED is incompatible with DirectComposition output:
	// Qt's DComp target/visual creation succeeds but nothing actually
	// composites through the swap chain, breaking alpha. Qt sets the
	// flag for frameless translucent windows (qwindowswindow.cpp
	// setWindowLayered) before it realises DComp is in use, so we
	// intercept the WM_STYLECHANGING that SetWindowLongPtr emits and
	// strip the bit synchronously.
	if (msg == WM_STYLECHANGING
		&& wParam == GWL_EXSTYLE
		&& lParam != 0) {
		auto *ss = reinterpret_cast<STYLESTRUCT *>(lParam);
		ss->styleNew &= ~LONG(WS_EX_LAYERED);
	}
	if (msg == WM_NCDESTROY) {
		::RemoveWindowSubclass(
			hwnd,
			&StripLayeredExStyleSubclass,
			uIdSubclass);
	}
	return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

void SurfaceRhi::installExStyleFilterWin() {
	if (_exStyleFilterHwnd) {
		return;
	}
	const auto tlw = window();
	if (!tlw || !tlw->testAttribute(Qt::WA_TranslucentBackground)) {
		return;
	}
	const auto wh = tlw->windowHandle();
	if (!wh || wh->surfaceType() != QSurface::Direct3DSurface) {
		return;
	}
	const auto hwnd = reinterpret_cast<HWND>(wh->winId());
	if (!hwnd) {
		return;
	}
	if (!::SetWindowSubclass(
			hwnd,
			&StripLayeredExStyleSubclass,
			kExStyleSubclassId,
			0)) {
		return;
	}
	_exStyleFilterHwnd = hwnd;
	// Clear WS_EX_LAYERED if Qt set it before our subclass was attached,
	// matching what the subclass does for subsequent style changes.
	const auto exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if (exStyle & WS_EX_LAYERED) {
		::SetWindowLongPtrW(
			hwnd,
			GWL_EXSTYLE,
			exStyle & ~LONG_PTR(WS_EX_LAYERED));
	}
}

void SurfaceRhi::removeExStyleFilterWin() {
	if (!_exStyleFilterHwnd) {
		return;
	}
	::RemoveWindowSubclass(
		_exStyleFilterHwnd,
		&StripLayeredExStyleSubclass,
		kExStyleSubclassId);
	_exStyleFilterHwnd = nullptr;
}
#endif // Q_OS_WIN

void SurfaceRhi::ensureBackingStoreRhi() {
	if (_backingStoreConfigured) {
		return;
	}
	_backingStoreConfigured = true;

	const auto tlw = window();
	if (!tlw) {
		return;
	}
	const auto wh = tlw->windowHandle();
	if (!wh) {
		return;
	}
	auto *bs = tlw->backingStore();
	if (!bs) {
		return;
	}
	auto *handle = bs->handle();
	if (!handle) {
		return;
	}
	QPlatformBackingStoreRhiConfig config;
	config.setEnabled(true);
#ifdef Q_OS_MAC
	if (::Platform::MetalSupported()) {
		config.setApi(QPlatformBackingStoreRhiConfig::Metal);
		if (wh->surfaceType() != QSurface::MetalSurface) {
			wh->setSurfaceType(QSurface::MetalSurface);
		}
	} else {
		config.setApi(QPlatformBackingStoreRhiConfig::OpenGL);
	}
#elif defined(Q_OS_WIN)
	// Mirror the Api choice used by setApi() above so the top-level
	// backing store composites through the same QRhi backend as the
	// QRhiWidget renders to.
	config.setApi(::Platform::IsWindows8OrGreater()
		? QPlatformBackingStoreRhiConfig::D3D11
		: QPlatformBackingStoreRhiConfig::OpenGL);
#else
	config.setApi(QPlatformBackingStoreRhiConfig::OpenGL);
#endif
	handle->createRhi(wh, config);
}

bool SurfaceRhi::eventHook(QEvent *e) {
	if (e->type() == QEvent::Show
		|| e->type() == QEvent::Paint
		|| e->type() == QEvent::Resize) {
#ifdef Q_OS_WIN
		installExStyleFilterWin();
#endif // Q_OS_WIN
		ensureBackingStoreRhi();
	}
	return RpWidgetBase<QRhiWidget, SurfaceRhiTraits>::eventHook(e);
}

void SurfaceRhi::initialize(QRhiCommandBuffer *cb) {
	if (const auto r = rhiRenderer()) {
		r->initialize(rhi(), renderTarget(), cb);
	}
}

void SurfaceRhi::render(QRhiCommandBuffer *cb) {
	if (!updatesEnabled() || size().isEmpty()) {
		return;
	}
	if (const auto r = rhiRenderer()) {
		r->render(rhi(), renderTarget(), cb);
	}
}

void SurfaceRhi::releaseResources() {
	if (const auto r = rhiRenderer()) {
		r->releaseResources();
	}
}

Rhi::Renderer *SurfaceRhi::rhiRenderer() const {
	return dynamic_cast<Rhi::Renderer*>(_renderer.get());
}
#endif // Qt >= 6.7

std::unique_ptr<RpWidgetWrap> CreateSurfaceRhi(
		QWidget *parent,
		std::unique_ptr<Renderer> renderer) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	return std::make_unique<SurfaceRhi>(
		parent,
		std::move(renderer));
#else // Qt >= 6.7
	LOG(("QRhi: Not available (Qt < 6.7), falling back to raster."));
	return nullptr;
#endif // Qt >= 6.7
}

} // namespace Ui::GL
