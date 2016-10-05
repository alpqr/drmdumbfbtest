#include <QGuiApplication>
#include <QTimer>
#include <QtKmsSupport/private/qkmsdevice_p.h>
#include <QtCore/private/qcore_unix_p.h>
#include <sys/mman.h>

class Device : public QKmsDevice
{
public:
    Device(QKmsScreenConfig *screenConfig);
    bool open() override;
    void close() override;
    void *nativeDisplay() const override;
    QPlatformScreen *createScreen(const QKmsOutput &output) override;
    void registerScreen(QPlatformScreen *screen,
                        const QPoint &virtualPos,
                        const QList<QPlatformScreen *> &virtualSiblings) override;

    void createFramebuffers();
    void destroyFramebuffers();

    struct Framebuffer {
        Framebuffer() : handle(0), pitch(0), size(0), fb(0), p(MAP_FAILED) { }
        uint32_t handle;
        uint32_t pitch;
        uint64_t size;
        uint32_t fb;
        void *p;
    };

    struct Output {
        QKmsOutput kmsOutput;
        Framebuffer fb;
    };

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

void Device::createFramebuffers()
{
    for (Output &output : m_outputs) {
        drmModeModeInfo &modeInfo(output.kmsOutput.modes[output.kmsOutput.mode]);
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
            return;
        }

        output.fb.handle = creq.handle;
        output.fb.pitch = creq.pitch;
        output.fb.size = creq.size;
        qDebug("Got a dumb buffer for size %dx%d, handle %u, pitch %u, size %lu", w, h,
               output.fb.handle, output.fb.pitch, output.fb.size);

        if (drmModeAddFB(fd(), w, h, 24, 32, output.fb.pitch, output.fb.handle, &output.fb.fb) == -1) {
            qErrnoWarning(errno, "Failed to add FB");
            return;
        }

        drm_mode_map_dumb mreq = {
            output.fb.handle,
            0, 0
        };
        if (drmIoctl(fd(), DRM_IOCTL_MODE_MAP_DUMB, &mreq) == -1) {
            qErrnoWarning(errno, "Failed to map dumb buffer");
            return;
        }
        output.fb.p = mmap(0, output.fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd(), mreq.offset);
        if (output.fb.p == MAP_FAILED) {
            qErrnoWarning(errno, "Failed to mmap dumb buffer");
            return;
        }

        qDebug("FB is %u, mapped at %p", output.fb.fb, output.fb.p);
        memset(output.fb.p, 0, output.fb.size);

        if (drmModeSetCrtc(fd(), output.kmsOutput.crtc_id, output.fb.fb, 0, 0,
                           &output.kmsOutput.connector_id, 1, &modeInfo) == -1) {
            qErrnoWarning(errno, "Failed to set mode");
            return;
        }

        output.kmsOutput.mode_set = true; // have cleanup() to restore the mode
        output.kmsOutput.setPowerState(this, QPlatformScreen::PowerStateOn);
    }
}

void Device::destroyFramebuffers()
{
    for (Output &output : m_outputs) {
        if (output.fb.p != MAP_FAILED)
            munmap(output.fb.p, output.fb.size);
        if (output.fb.fb) {
            if (drmModeRmFB(fd(), output.fb.fb) == -1)
                qErrnoWarning("Failed to remove fb");
        }
        if (output.fb.handle) {
            drm_mode_destroy_dumb dreq = { output.fb.handle };
            if (drmIoctl(fd(), DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) == -1)
                qErrnoWarning(errno, "Failed to destroy dumb buffer %u", output.fb.handle);
        }
        output.fb = Framebuffer();
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
    for (Device::Output &output : m_device->m_outputs) {
        if (output.fb.p == MAP_FAILED)
            continue;
        quint32 *ip = static_cast<quint32 *>(output.fb.p);
        for (size_t i = 0; i < output.fb.size / 4; ++i)
            *ip++ = (m_r << 16) | (m_g << 8) | (m_b);
        m_r += 1;
        m_g += 2;
        m_b += 3;
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "linuxfb:nographicsmodeswitch"); // something whose init won't interfere with us
    QGuiApplication app(argc, argv);

    DumbBufferRenderer r;

    const int t = 10;
    qDebug("Running for %d seconds", t);
    QTimer::singleShot(t * 1000, &app, &QCoreApplication::quit);
    return app.exec();
}
