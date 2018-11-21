// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QOffscreenSurface>
#include <QOpenGLWindow>
#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <fmt/format.h>
#include "citra_qt/bootmanager.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/settings.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/motion_emu.h"
#include "network/network.h"

EmuThread::EmuThread(GRenderWindow* render_window) : render_window(render_window) {}

void EmuThread::run() {
    render_window->MakeCurrent();

    MicroProfileOnThreadCreate("EmuThread");

    stop_run = false;

    // Holds whether the cpu was running during the last iteration,
    // so that the DebugModeLeft signal can be emitted before the
    // next execution step.
    bool was_active = false;
    while (!stop_run) {
        if (running) {
            if (!was_active)
                emit DebugModeLeft();

            Core::System::ResultStatus result = Core::System::GetInstance().RunLoop();
            if (result == Core::System::ResultStatus::ShutdownRequested) {
                // Notify frontend we shutdown
                emit ErrorThrown(result, "");
                // End emulation execution
                break;
            }
            if (result != Core::System::ResultStatus::Success) {
                this->SetRunning(false);
                emit ErrorThrown(result, Core::System::GetInstance().GetStatusDetails());
            }

            was_active = running || exec_step;
            if (!was_active && !stop_run)
                emit DebugModeEntered();
        } else if (exec_step) {
            if (!was_active)
                emit DebugModeLeft();

            exec_step = false;
            Core::System::GetInstance().SingleStep();
            emit DebugModeEntered();
            yieldCurrentThread();

            was_active = false;
        } else {
            std::unique_lock<std::mutex> lock(running_mutex);
            running_cv.wait(lock, [this] { return IsRunning() || exec_step || stop_run; });
        }
    }

    // Shutdown the core emulation
    Core::System::GetInstance().Shutdown();

#if MICROPROFILE_ENABLED
    MicroProfileOnThreadExit();
#endif

    render_window->moveContext();
}

class GGLContext : public GraphicsContext {
public:
    explicit GGLContext(QOpenGLContext* shared_context) : surface() {
        context = std::make_unique<QOpenGLContext>(shared_context);
        surface.setFormat(shared_context->format());
        surface.create();
    }

    void MakeCurrent() override {
        context->makeCurrent(&surface);
    }

    void DoneCurrent() override {
        context->doneCurrent();
    }

    void SwapBuffers() override {}

private:
    std::unique_ptr<QOpenGLContext> context;
    QOffscreenSurface surface;
};

// This class overrides paintEvent and resizeEvent to prevent the GUI thread from stealing GL
// context.
// The corresponding functionality is handled in EmuThread instead
class GGLWidgetInternal : public QOpenGLWindow {
public:
    GGLWidgetInternal(GRenderWindow* parent, QOpenGLContext* shared_context)
        : QOpenGLWindow(shared_context), parent(parent) {}

    void paintEvent(QPaintEvent* ev) override {
        if (do_painting) {
            QPainter painter(this);
        }
    }

    void resizeEvent(QResizeEvent* ev) override {
        parent->OnClientAreaResized(ev->size().width(), ev->size().height());
        parent->OnFramebufferSizeChanged();
    }

    void DisablePainting() {
        do_painting = false;
    }
    void EnablePainting() {
        do_painting = true;
    }

private:
    GRenderWindow* parent;
    bool do_painting;
};

GRenderWindow::GRenderWindow(QWidget* parent, EmuThread* emu_thread)
    : QWidget(parent), child(nullptr), context(nullptr), emu_thread(emu_thread) {

    setWindowTitle(QStringLiteral("Citra %1 | %2-%3")
                       .arg(Common::g_build_name, Common::g_scm_branch, Common::g_scm_desc));
    setAttribute(Qt::WA_AcceptTouchEvents);

    InputCommon::Init();
}

GRenderWindow::~GRenderWindow() {
    InputCommon::Shutdown();
}

void GRenderWindow::moveContext() {
    DoneCurrent();

    // If the thread started running, move the GL Context to the new thread. Otherwise, move it
    // back.
    auto thread = (QThread::currentThread() == qApp->thread() && emu_thread != nullptr)
                      ? emu_thread
                      : qApp->thread();

    context->moveToThread(thread);
}

void GRenderWindow::SwapBuffers() {
    // In our multi-threaded QWidget use case we shouldn't need to call `makeCurrent`,
    // since we never call `doneCurrent` in this thread.
    // However:
    // - The Qt debug runtime prints a bogus warning on the console if `makeCurrent` wasn't called
    // since the last time `swapBuffers` was executed;
    // - On macOS, if `makeCurrent` isn't called explicitely, resizing the buffer breaks.
    context->makeCurrent(child);

    context->swapBuffers(child);
}

void GRenderWindow::MakeCurrent() {
    context->makeCurrent(child);
}

void GRenderWindow::DoneCurrent() {
    context->doneCurrent();
}

void GRenderWindow::PollEvents() {}

// On Qt 5.0+, this correctly gets the size of the framebuffer (pixels).
//
// Older versions get the window size (density independent pixels),
// and hence, do not support DPI scaling ("retina" displays).
// The result will be a viewport that is smaller than the extent of the window.
void GRenderWindow::OnFramebufferSizeChanged() {
    // Screen changes potentially incur a change in screen DPI, hence we should update the
    // framebuffer size
    qreal pixelRatio = windowPixelRatio();
    unsigned width = child->QPaintDevice::width() * pixelRatio;
    unsigned height = child->QPaintDevice::height() * pixelRatio;
    UpdateCurrentFramebufferLayout(width, height);
}

void GRenderWindow::BackupGeometry() {
    geometry = ((QWidget*)this)->saveGeometry();
}

void GRenderWindow::RestoreGeometry() {
    // We don't want to back up the geometry here (obviously)
    QWidget::restoreGeometry(geometry);
}

void GRenderWindow::restoreGeometry(const QByteArray& geometry) {
    // Make sure users of this class don't need to deal with backing up the geometry themselves
    QWidget::restoreGeometry(geometry);
    BackupGeometry();
}

QByteArray GRenderWindow::saveGeometry() {
    // If we are a top-level widget, store the current geometry
    // otherwise, store the last backup
    if (parent() == nullptr)
        return ((QWidget*)this)->saveGeometry();
    else
        return geometry;
}

qreal GRenderWindow::windowPixelRatio() const {
    // windowHandle() might not be accessible until the window is displayed to screen.
    return windowHandle() ? windowHandle()->screen()->devicePixelRatio() : 1.0f;
}

std::pair<unsigned, unsigned> GRenderWindow::ScaleTouch(const QPointF pos) const {
    const qreal pixel_ratio = windowPixelRatio();
    return {static_cast<unsigned>(std::max(std::round(pos.x() * pixel_ratio), qreal{0.0})),
            static_cast<unsigned>(std::max(std::round(pos.y() * pixel_ratio), qreal{0.0}))};
}

void GRenderWindow::closeEvent(QCloseEvent* event) {
    emit Closed();
    QWidget::closeEvent(event);
}

void GRenderWindow::keyPressEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->PressKey(event->key());
}

void GRenderWindow::keyReleaseEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->ReleaseKey(event->key());
}

void GRenderWindow::mousePressEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchBeginEvent

    auto pos = event->pos();
    if (event->button() == Qt::LeftButton) {
        const auto [x, y] = ScaleTouch(pos);
        this->TouchPressed(x, y);
    } else if (event->button() == Qt::RightButton) {
        InputCommon::GetMotionEmu()->BeginTilt(pos.x(), pos.y());
    }
}

void GRenderWindow::mouseMoveEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchUpdateEvent

    auto pos = event->pos();
    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
    InputCommon::GetMotionEmu()->Tilt(pos.x(), pos.y());
}

void GRenderWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchEndEvent

    if (event->button() == Qt::LeftButton)
        this->TouchReleased();
    else if (event->button() == Qt::RightButton)
        InputCommon::GetMotionEmu()->EndTilt();
}

void GRenderWindow::TouchBeginEvent(const QTouchEvent* event) {
    // TouchBegin always has exactly one touch point, so take the .first()
    const auto [x, y] = ScaleTouch(event->touchPoints().first().pos());
    this->TouchPressed(x, y);
}

void GRenderWindow::TouchUpdateEvent(const QTouchEvent* event) {
    QPointF pos;
    int active_points = 0;

    // average all active touch points
    for (const auto tp : event->touchPoints()) {
        if (tp.state() & (Qt::TouchPointPressed | Qt::TouchPointMoved | Qt::TouchPointStationary)) {
            active_points++;
            pos += tp.pos();
        }
    }

    pos /= active_points;

    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
}

void GRenderWindow::TouchEndEvent() {
    this->TouchReleased();
}

bool GRenderWindow::event(QEvent* event) {
    if (event->type() == QEvent::TouchBegin) {
        TouchBeginEvent(static_cast<QTouchEvent*>(event));
        return true;
    } else if (event->type() == QEvent::TouchUpdate) {
        TouchUpdateEvent(static_cast<QTouchEvent*>(event));
        return true;
    } else if (event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
        TouchEndEvent();
        return true;
    }

    return QWidget::event(event);
}

void GRenderWindow::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    InputCommon::GetKeyboard()->ReleaseAllKeys();
}

void GRenderWindow::OnClientAreaResized(unsigned width, unsigned height) {
    NotifyClientAreaSizeChanged(std::make_pair(width, height));
}

std::unique_ptr<GraphicsContext> GRenderWindow::CreateSharedContext() const {
    return std::make_unique<GGLContext>(shared_context.get());
}

void GRenderWindow::InitRenderTarget() {
    if (shared_context) {
        shared_context.reset();
    }

    if (context) {
        context.reset();
    }

    if (child) {
        delete child;
    }

    if (layout()) {
        delete layout();
    }

    // TODO: One of these flags might be interesting: WA_OpaquePaintEvent, WA_NoBackground,
    // WA_DontShowOnScreen, WA_DeleteOnClose
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    // TODO: expose a setting for buffer value (ie default/single/double/triple)
    fmt.setSwapBehavior(QSurfaceFormat::DefaultSwapBehavior);
    // fmt.setSwapBehavior(QSurfaceFormat::SingleBuffer);
    shared_context = std::make_unique<QOpenGLContext>();
    shared_context->setFormat(fmt);
    shared_context->create();
    context = std::make_unique<QOpenGLContext>();
    context->setShareContext(shared_context.get());
    context->setFormat(fmt);
    context->create();
    fmt.setSwapInterval(Settings::values.use_vsync);

    child = new GGLWidgetInternal(this, shared_context.get());
    QWidget* container = QWidget::createWindowContainer(child, this);
    QBoxLayout* layout = new QHBoxLayout(this);

    resize(Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight);
    child->resize(Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight);
    container->resize(Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight);
    layout->addWidget(container);
    layout->setMargin(0);
    setLayout(layout);

    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);

    OnFramebufferSizeChanged();
    NotifyClientAreaSizeChanged(std::pair<unsigned, unsigned>(child->width(), child->height()));

    BackupGeometry();

    // show causes the window to actually be created and the gl context as well
    show();
}

void GRenderWindow::OnMinimalClientAreaChangeRequest(
    const std::pair<unsigned, unsigned>& minimal_size) {
    setMinimumSize(minimal_size.first, minimal_size.second);
}

void GRenderWindow::OnEmulationStarting(EmuThread* emu_thread) {
    this->emu_thread = emu_thread;
    child->DisablePainting();
}

void GRenderWindow::OnEmulationStopping() {
    emu_thread = nullptr;
    child->EnablePainting();
}

void GRenderWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

    // windowHandle() is not initialized until the Window is shown, so we connect it here.
    connect(windowHandle(), &QWindow::screenChanged, this, &GRenderWindow::OnFramebufferSizeChanged,
            Qt::UniqueConnection);
}
