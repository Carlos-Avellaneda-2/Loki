#pragma once

#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <map>
#include <vector>

class QAction;
class QWidget;

namespace loki::ui {

// Central registry of keyboard shortcuts. Every bindable command is a QAction registered with a
// stable id, a context (the panel it works in, or "Application") and a default key. User changes
// are stored in QSettings ("shortcuts/<id>") and applied to every action with that id.
class ShortcutManager final : public QObject {
    Q_OBJECT

public:
    static constexpr auto kApplicationContext = "Application";

    struct Entry {
        QString id;
        QString context;
        QString text;
        QKeySequence defaultKey;
        QKeySequence key;
    };

    explicit ShortcutManager(QObject* parent = nullptr);

    // Registers an existing action (e.g. a menu action) and applies the stored or default key.
    QAction* bind(QAction* action, const QString& id, const QString& context, const QKeySequence& defaultKey);

    // Creates an action owned by `owner`, active while the owner (or a child) has focus, and binds it.
    QAction* create(QWidget* owner, const QString& id, const QString& context, const QString& text,
                    const QKeySequence& defaultKey, std::function<void()> handler);

    [[nodiscard]] std::vector<Entry> entries() const;
    [[nodiscard]] QKeySequence shortcut(const QString& id) const;
    [[nodiscard]] QKeySequence defaultShortcut(const QString& id) const;

    // Applies and persists (an empty sequence removes the shortcut).
    void setShortcut(const QString& id, const QKeySequence& key);
    void resetAll();

    // Pairs of ids that share a key in the same context (or one of them is application-wide).
    [[nodiscard]] static std::vector<std::pair<QString, QString>> conflicts(const std::vector<Entry>& entries);

signals:
    void shortcutsChanged();

private:
    struct Binding {
        Entry entry;
        QList<QPointer<QAction>> actions;
    };
    void apply(Binding& binding);

    std::map<QString, Binding> m_bindings;
};

} // namespace loki::ui
