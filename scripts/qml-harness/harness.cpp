// Offscreen QQuickView render harness for the whisperbox view.
// Loads the REAL module/Main.qml with a mock `logos` context property that
// serves a fixture snapshot, lets the poll timers fire, then saves a screenshot
// and reports any QML errors. Catches runtime QML errors qmllint cannot.
//
// Build: see render.sh (compiles against the nix-store Qt6 the design system
// was built with).
// Usage: harness <Main.qml> <fixture.json> [out.png]

#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickView>
#include <QQuickItem>
#include <QWindow>
#include <QImage>
#include <QTimer>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QMessageLogger>
#include <QString>
#include <QVariantList>
#include <QJsonArray>
#include <QJSValue>
// QTest only declares the QWindow mouse helpers when QT_GUI_LIB is defined.
#ifndef QT_GUI_LIB
#define QT_GUI_LIB
#endif
#include <QTest>

#include <cstdio>

static int g_qmlErrors = 0;
static bool g_viewError = false;

static void messageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
    QString line = msg;
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        // Count real QML/runtime errors (typos, missing types/props, binding loops).
        if (line.contains("is not a type") || line.contains("Cannot assign to non-existent property")
            || line.contains("TypeError") || line.contains("ReferenceError")
            || line.contains("qrc:/") && line.contains("error")) {
            g_qmlErrors++;
        }
    }
    fprintf(stderr, "[%s] %s\n",
            type == QtDebugMsg ? "D" : type == QtInfoMsg ? "I" : type == QtWarningMsg ? "W" : "E",
            line.toUtf8().constData());
}

class MockLogos : public QObject {
    Q_OBJECT
public:
    explicit MockLogos(const QString &fixture, QObject *parent = nullptr)
        : QObject(parent), m_fixture(fixture) {}

    Q_INVOKABLE QString callModule(const QString &mod, const QString &method, const QVariantList &args) {
        // Every call is logged so render.sh can assert the view's call contract
        // (method name + argument count/shape) against the real core API.
        if (method != "snapshot") {
            QStringList a; for (const auto &v : args) a << v.toString();
            fprintf(stderr, "CALL %s %s argc=%d args=%s\n", mod.toUtf8().constData(), method.toUtf8().constData(),
                    (int)args.size(), a.join(" | ").toUtf8().constData());
        }
        // Crema: every call goes to the REAL crema_core in a logosctl session (CREMA_CTL = ctl.sh + session),
        // so the view renders real state and real QR codes. Without CREMA_CTL: the fixture as the snapshot.
        const QByteArray ctl = qgetenv("CREMA_CTL");
        if (ctl.isEmpty()) return method == "snapshot" ? m_fixture : QString("{\"ok\":true}");
        QStringList parts = QString::fromLocal8Bit(ctl).split(' ');
        QStringList pa = parts.mid(1); pa << "call" << mod << method;
        for (const auto &v : args) pa << ("str:" + v.toString());
        QProcess pr; pr.start(parts[0], pa); pr.waitForFinished(30000);
        const QList<QByteArray> lines = pr.readAllStandardOutput().trimmed().split('\n');
        const QJsonObject o = QJsonDocument::fromJson(lines.isEmpty() ? QByteArray() : lines.last()).object();
        return o["result"].toString();
    }

    // The view must use this (non-blocking) path; answered on the next event-loop turn,
    // like the real host.
    Q_INVOKABLE void callModuleAsync(const QString &mod, const QString &method, const QVariantList &args, const QJSValue &cb, int timeoutMs = 0) {
        Q_UNUSED(timeoutMs);
        const QString r = callModule(mod, method, args);
        QJSValue c = cb;
        QTimer::singleShot(0, this, [c, r]() mutable { if (c.isCallable()) c.call({QJSValue(r)}); });
    }
    Q_INVOKABLE void onModuleEvent(const QString &mod, const QString &ev) {}

signals:
    void moduleEventReceived(const QString &mod, const QString &ev, const QString &data);

private:
    QString m_fixture;
};

#include "harness.moc"

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    qInstallMessageHandler(messageHandler);

    if (argc < 3) {
        fprintf(stderr, "usage: harness <Main.qml> <fixture.json> [out.png]\n");
        return 2;
    }
    QFile fx(argv[2]);
    if (!fx.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "cannot open fixture %s\n", argv[2]);
        return 2;
    }
    QString fixture = QString::fromUtf8(fx.readAll());

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(1280, 800);

    // NOTE: QQuickView owns its OWN QQmlEngine — context properties must be set
    // on view.engine(), not a separate engine, or the component never sees them.
    auto *logos = new MockLogos(fixture, &view);
    view.engine()->rootContext()->setContextProperty("logos", logos);

    QObject::connect(&view, &QQuickView::statusChanged, [](QQuickView::Status s) {
        if (s == QQuickView::Error) {
            g_viewError = true;
            fprintf(stderr, "RENDER FAIL: view status Error\n");
        }
    });
    view.setSource(QUrl::fromLocalFile(argv[1]));
    view.show();   // offscreen: still triggers proper exposure + scene-graph frames
    QTimer::singleShot(1000, [&view] {
        QQuickItem *r = view.rootObject();
        fprintf(stderr, "DEBUG: view=%dx%d root=%s w=%.0f h=%.0f\n",
                (int)view.width(), (int)view.height(), r ? "ok" : "null",
                r ? r->width() : -1, r ? r->height() : -1);
    });

    // WB_PROPS='{"selectedId":"form-x","showCreate":true}' sets root properties;
    // WB_INVOKE="fnA,fnB" then calls root QML functions in order (no args).
    QTimer::singleShot(600, [&view] {
        QQuickItem *r = view.rootObject(); if (!r) return;
        const QByteArray props = qgetenv("WB_PROPS");
        if (!props.isEmpty()) {
            const QJsonObject o = QJsonDocument::fromJson(props).object();
            for (auto it = o.begin(); it != o.end(); ++it)
                if (!r->setProperty(it.key().toUtf8().constData(), it.value().toVariant()))
                    { fprintf(stderr, "[E] WB_PROPS: no property %s\n", it.key().toUtf8().constData()); g_qmlErrors++; }
        }
        const QByteArray inv = qgetenv("WB_INVOKE");
        for (const QByteArray &fn : inv.split(',')) {
            if (fn.trimmed().isEmpty()) continue;
            if (!QMetaObject::invokeMethod(r, fn.trimmed().constData()))
                { fprintf(stderr, "[E] WB_INVOKE: no function %s\n", fn.constData()); g_qmlErrors++; }
        }
    });

    // WB_CLICKS="x,y;x,y" real mouse clicks (window coords), 400 ms apart, after
    // WB_PROPS/WB_INVOKE - exercises the actual delegates/handlers, not just functions.
    {
        const QList<QByteArray> clicks = qgetenv("WB_CLICKS").split(';');
        int t = 1000;
        for (const QByteArray &c : clicks) {
            const QList<QByteArray> xy = c.split(',');
            if (xy.size() != 2) continue;
            const QPointF p(xy[0].toDouble(), xy[1].toDouble());
            // QTest goes through the platform input path (hand-built QMouseEvents
            // via sendEvent are not delivered to MouseAreas on Qt 6.9 offscreen).
            QTimer::singleShot(t, [&view, p] { QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, p.toPoint()); });
            t += 400;
        }
    }
    // WB_DUMP="propA,propB": print root properties as JSON at the end (DUMP <name> <json>).
    QTimer::singleShot(4300, [&view] {
        QQuickItem *r = view.rootObject(); if (!r) return;
        for (const QByteArray &n : qgetenv("WB_DUMP").split(',')) {
            if (n.isEmpty()) continue;
            // var properties arrive as QJSValue; plain string/int/bool properties as themselves
            const QVariant raw = r->property(n.constData());
            const QJsonValue v = QJsonValue::fromVariant(raw.canConvert<QJSValue>() && raw.userType() == qMetaTypeId<QJSValue>()
                                                         ? raw.value<QJSValue>().toVariant() : raw);
            const QByteArray j = v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)
                               : v.isObject() ? QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)
                               : QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
            fprintf(stderr, "DUMP %s %s\n", n.constData(), j.constData());
        }
    });

    // Let the 2.5s poll + Qt.callLater deferrals run, then screenshot.
    QString outPath = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    QTimer::singleShot(4500, [&view, &outPath] {
        if (!outPath.isEmpty()) view.grabWindow().save(outPath);
        bool ok = (g_qmlErrors == 0) && !g_viewError;
        fprintf(stderr, "RENDER %s: qmlErrors=%d viewError=%d\n", ok ? "OK" : "FAIL", g_qmlErrors, (int)g_viewError);
        QGuiApplication::exit(ok ? 0 : 1);
    });
    QTimer::singleShot(20000, [] {
        fprintf(stderr, "RENDER TIMEOUT\n");
        QGuiApplication::exit(3);
    });

    return app.exec();
}
