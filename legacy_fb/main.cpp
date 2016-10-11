#include <QGuiApplication>
#include <QTimer>
#include <QRect>
#include <QDebug>
#include <QtCore/private/qcore_unix_p.h>
#include <sys/mman.h>
#include <linux/fb.h>

class Device
{
public:
    Device();
    bool open();
    void close();

    struct Framebuffer {
        Framebuffer()
            : size(0),
              pitch(0),
              p(MAP_FAILED),
              depth(0)
            { }
        uint64_t size;
        uint32_t pitch;
        void *p;
        int depth;
        QRect geom;
    };

    int fd;
    Framebuffer fb;

    uchar *screenStart() {
        return static_cast<uchar *>(fb.p) + fb.geom.y() * fb.pitch + fb.geom.x() * fb.depth / 8;
    }
};

Device::Device()
    : fd(-1)
{
}

bool Device::open()
{
    const QString devicePath = QStringLiteral("/dev/fb0");
    fd = qt_safe_open(devicePath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        qErrnoWarning("Could not open device %s", qPrintable(devicePath));
        return false;
    }

    fb_fix_screeninfo finfo;
    fb_var_screeninfo vinfo;
    memset(&vinfo, 0, sizeof(vinfo));
    memset(&finfo, 0, sizeof(finfo));
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        qErrnoWarning(errno, "Error reading fixed information");
        return false;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo)) {
        qErrnoWarning(errno, "Error reading variable information");
        return false;
    }

    fb.size = finfo.smem_len;
    fb.pitch = finfo.line_length;
    fb.p = mmap(0, fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb.p == MAP_FAILED) {
        qErrnoWarning(errno, "Failed to mmap framebuffer");
        return false;
    }

    qDebug("Mapped framebuffer at %p, sizeo %lu, stride %u", fb.p, fb.size, fb.pitch);

    fb.depth = vinfo.bits_per_pixel;
    if (fb.depth == 24) {
        fb.depth = vinfo.red.length + vinfo.green.length + vinfo.blue.length;
        if (fb.depth <= 0)
            fb.depth = 24;
    } else if (fb.depth == 16) {
        fb.depth = vinfo.red.length + vinfo.green.length + vinfo.blue.length;
        if (fb.depth <= 0)
            fb.depth = 16;
    }
    qDebug("Depth is %d", fb.depth);

    if (fb.depth != 32) {
        qWarning("Depths other than 32 will not work for now");
        return false;
    }

    fb.geom = QRect(vinfo.xoffset, vinfo.yoffset, vinfo.xres, vinfo.yres);
    qDebug() << fb.geom;

    return true;
}

void Device::close()
{
    if (fb.p != MAP_FAILED)
        munmap(fb.p, fb.size);
    fb = Framebuffer();
    if (fd != -1) {
        qt_safe_close(fd);
        fd = -1;
    }
}

class FbRenderer : public QObject
{
public:
    FbRenderer();
    ~FbRenderer();

private:
    void update();

    Device *m_device;
    QTimer m_timer;
    int m_r = 0, m_g = 0, m_b = 0;
};

FbRenderer::FbRenderer()
{
    m_device = new Device;
    if (!m_device->open()) {
        qWarning("Failed to open framebuffer device");
        return;
    }

    m_timer.setInterval(16);
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &FbRenderer::update);
    m_timer.start();
}

FbRenderer::~FbRenderer()
{
    if (m_device) {
        qDebug("Closing down");
        m_device->close();
        delete m_device;
    }
}

void FbRenderer::update()
{
    if (m_device->fb.p == MAP_FAILED)
        return;

    for (int y = 0; y < m_device->fb.geom.height(); ++y) {
        quint32 *ip = reinterpret_cast<quint32 *>(m_device->screenStart() + y * m_device->fb.pitch);
        for (int x = 0; x < m_device->fb.geom.width(); ++x)
            *ip++ = (m_r << 16) | (m_g << 8) | (m_b);
    }
    m_r += 1;
    m_g += 2;
    m_b += 3;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "linuxfb:nographicsmodeswitch"); // something whose init won't interfere with us
    qputenv("QT_LOGGING_RULES", "qt.qpa.*=true");
    QGuiApplication app(argc, argv);

    FbRenderer r;

    const int t = 10;
    qDebug("Running for %d seconds", t);
    QTimer::singleShot(t * 1000, &app, &QCoreApplication::quit);
    return app.exec();
}
