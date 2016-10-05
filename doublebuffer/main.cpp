#include <QGuiApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <QtKmsSupport/private/qkmsdevice_p.h>
#include <QtCore/private/qcore_unix_p.h>
#include <sys/mman.h>

static const int BUFFER_COUNT = 2;

class Device : public QKmsDevice
{
public:
    struct Framebuffer {
        Framebuffer() : handle(0), pitch(0), size(0), fb(0), p(MAP_FAILED) { }
        uint32_t handle;
        uint32_t pitch;
        uint64_t size;
        uint32_t fb;
        void *p;
    };

    struct Output {
        Output() : backFb(0), flipped(false) { }
        QKmsOutput kmsOutput;
        Framebuffer fb[2];
        int backFb;
        bool flipped;
    };

    Device(QKmsScreenConfig *screenConfig);
    bool open() override;
    void close() override;

    void createFramebuffers();
    void destroyFramebuffers();
    void setMode();

    void swapBuffers(Output *output);

    QVector<Output> *outputs() { return &m_outputs; }

private:
    void *nativeDisplay() const override;
    QPlatformScreen *createScreen(const QKmsOutput &output) override;
    void registerScreen(QPlatformScreen *screen,
                        const QPoint &virtualPos,
                        const QList<QPlatformScreen *> &virtualSiblings) override;

    bool createFramebuffer(Device::Output *output, int bufferIdx);
    void destroyFramebuffer(Device::Output *output, int bufferIdx);

    static void pageFlipHandler(int fd, unsigned int sequence,
                                unsigned int tv_sec, unsigned int tv_usec, void *user_data);

    QVector<Output> m_outputs;
};

Device::Device(QKmsScreenConfig *screenConfig)
    : QKmsDevice(screenConfig, QStringLiteral("/dev/dri/card0"))
{
}

bool Device::open()
{
    int fd = qt_safe_open(devicePath().toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        qErrnoWarning("Could not open DRM device %s", qPrintable(devicePath()));
        return false;
    }
    uint64_t hasDumbBuf = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &hasDumbBuf) == -1 || !hasDumbBuf) {
        qWarning("Dumb buffers not supported");
        qt_safe_close(fd);
        return false;
    }
    setFd(fd);
    return true;
}

void Device::close()
{
    for (Output &output : m_outputs)
        output.kmsOutput.cleanup(this); // restore mode

    m_outputs.clear();

    if (fd() != -1) {
        qt_safe_close(fd());
        setFd(-1);
    }
}

void *Device::nativeDisplay() const
{
    // never called
    return nullptr;
}

QPlatformScreen *Device::createScreen(const QKmsOutput &output)
{
    qDebug("Got a new output: %s", qPrintable(output.name));
    Output o;
    o.kmsOutput = output;
    m_outputs.append(o);
    return nullptr; // no platformscreen, we are not a platform plugin
}

void Device::registerScreen(QPlatformScreen *screen,
                            const QPoint &virtualPos,
                            const QList<QPlatformScreen *> &virtualSiblings)
{
    // never called
    Q_UNUSED(screen);
    Q_UNUSED(virtualPos);
    Q_UNUSED(virtualSiblings);
}

bool Device::createFramebuffer(Device::Output *output, int bufferIdx)
{
    drmModeModeInfo &modeInfo(output->kmsOutput.modes[output->kmsOutput.mode]);
    const uint32_t w = modeInfo.hdisplay;
    const uint32_t h = modeInfo.vdisplay;
    drm_mode_create_dumb creq = {
        h,
        w,
        32,
        0, 0, 0, 0
    };
    if (drmIoctl(fd(), DRM_IOCTL_MODE_CREATE_DUMB, &creq) == -1) {
        qErrnoWarning(errno, "Failed to create dumb buffer");
        return false;
    }

    Framebuffer &fb(output->fb[bufferIdx]);
    fb.handle = creq.handle;
    fb.pitch = creq.pitch;
    fb.size = creq.size;
    qDebug("Got a dumb buffer for size %dx%d, handle %u, pitch %u, size %lu", w, h, fb.handle, fb.pitch, fb.size);

    if (drmModeAddFB(fd(), w, h, 24, 32, fb.pitch, fb.handle, &fb.fb) == -1) {
        qErrnoWarning(errno, "Failed to add FB");
        return false;
    }

    drm_mode_map_dumb mreq = {
        fb.handle,
        0, 0
    };
    if (drmIoctl(fd(), DRM_IOCTL_MODE_MAP_DUMB, &mreq) == -1) {
        qErrnoWarning(errno, "Failed to map dumb buffer");
        return false;
    }
    fb.p = mmap(0, fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd(), mreq.offset);
    if (fb.p == MAP_FAILED) {
        qErrnoWarning(errno, "Failed to mmap dumb buffer");
        return false;
    }

    qDebug("FB is %u, mapped at %p", fb.fb, fb.p);
    memset(fb.p, 0, fb.size);

    return true;
}

void Device::createFramebuffers()
{
    for (Output &output : m_outputs) {
        for (int i = 0; i < BUFFER_COUNT; ++i) {
            if (!createFramebuffer(&output, i))
                return;
        }
        output.backFb = 0;
        output.flipped = false;
    }
}

void Device::destroyFramebuffer(Device::Output *output, int bufferIdx)
{
    Framebuffer &fb(output->fb[bufferIdx]);
    if (fb.p != MAP_FAILED)
        munmap(fb.p, fb.size);
    if (fb.fb) {
        if (drmModeRmFB(fd(), fb.fb) == -1)
            qErrnoWarning("Failed to remove fb");
    }
    if (fb.handle) {
        drm_mode_destroy_dumb dreq = { fb.handle };
        if (drmIoctl(fd(), DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) == -1)
            qErrnoWarning(errno, "Failed to destroy dumb buffer %u", fb.handle);
    }
    fb = Framebuffer();
}

void Device::destroyFramebuffers()
{
    for (Output &output : m_outputs) {
        for (int i = 0; i < BUFFER_COUNT; ++i)
            destroyFramebuffer(&output, i);
    }
}

void Device::setMode()
{
    for (Output &output : m_outputs) {
        drmModeModeInfo &modeInfo(output.kmsOutput.modes[output.kmsOutput.mode]);
        if (drmModeSetCrtc(fd(), output.kmsOutput.crtc_id, output.fb[0].fb, 0, 0,
                           &output.kmsOutput.connector_id, 1, &modeInfo) == -1) {
            qErrnoWarning(errno, "Failed to set mode");
            return;
        }

        output.kmsOutput.mode_set = true; // have cleanup() to restore the mode
        output.kmsOutput.setPowerState(this, QPlatformScreen::PowerStateOn);
    }
}

void Device::pageFlipHandler(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *user_data)
{
    Q_UNUSED(fd);
    Q_UNUSED(sequence);
    Q_UNUSED(tv_sec);
    Q_UNUSED(tv_usec);

    Device::Output *output = static_cast<Device::Output *>(user_data);
    output->backFb = (output->backFb + 1) % BUFFER_COUNT;
}

//QElapsedTimer t;

void Device::swapBuffers(Output *output)
{
    if (output->flipped) {
        const int fbIdx = output->backFb;
        while (output->backFb == fbIdx) {
            drmEventContext drmEvent = {
                DRM_EVENT_CONTEXT_VERSION,
                nullptr,
                pageFlipHandler
            };
            // Blocks until there is something to read on the drm fd
            // and calls back pageFlipHandler once the flip completes.
            drmHandleEvent(fd(), &drmEvent);
        }
    } else {
        output->flipped = true;
    }

//    qDebug("flipping to %d elapsed %lld", output->backFb, t.restart());
    Framebuffer &fb(output->fb[output->backFb]);
    if (drmModePageFlip(fd(), output->kmsOutput.crtc_id, fb.fb, DRM_MODE_PAGE_FLIP_EVENT, output) == -1) {
        qErrnoWarning(errno, "Page flip failed");
        return;
    }
}

class DumbBufferRenderer : public QObject
{
public:
    DumbBufferRenderer();
    ~DumbBufferRenderer();

private:
    void update();

    QKmsScreenConfig m_screenConfig;
    Device *m_device;
    QTimer m_timer;
    int m_r = 0, m_g = 0, m_b = 0;
};

DumbBufferRenderer::DumbBufferRenderer()
{
    m_device = new Device(&m_screenConfig);
    if (!m_device->open()) {
        qWarning("Failed to open DRM device");
        return;
    }
    // Discover outputs. Calls back Device::createScreen().
    m_device->createScreens();
    // Now off to dumb buffer specifics.
    m_device->createFramebuffers();
    // Do the modesetting.
    m_device->setMode();

    m_timer.setInterval(16);
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &DumbBufferRenderer::update);
    m_timer.start();
}

DumbBufferRenderer::~DumbBufferRenderer()
{
    if (m_device) {
        qDebug("Closing down");
        m_device->destroyFramebuffers();
        m_device->close();
        delete m_device;
    }
}

void DumbBufferRenderer::update()
{
    for (Device::Output &output : *m_device->outputs()) {
        void *p = output.fb[output.backFb].p;
        if (p == MAP_FAILED)
            continue;
        quint32 *ip = static_cast<quint32 *>(p);
        for (size_t i = 0; i < output.fb[output.backFb].size / 4; ++i)
            *ip++ = (m_r << 16) | (m_g << 8) | (m_b);
        m_r += 1;
        m_g += 2;
        m_b += 3;
        m_device->swapBuffers(&output);
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "linuxfb:nographicsmodeswitch"); // something whose init won't interfere with us
    qputenv("QT_LOGGING_RULES", "qt.qpa.*=true");
    QGuiApplication app(argc, argv);

    DumbBufferRenderer r;

    const int t = 10;
    qDebug("Running for %d seconds", t);
    QTimer::singleShot(t * 1000, &app, &QCoreApplication::quit);
    return app.exec();
}
