#include "ui/shortcut_manager.h"

#include <QAction>
#include <QSettings>
#include <QWidget>

namespace loki::ui {

namespace {

QString settingsKey(const QString& id) {
    return QStringLiteral("shortcuts/") + id;
}

} // namespace

ShortcutManager::ShortcutManager(QObject* parent) : QObject(parent) {}

QAction* ShortcutManager::bind(QAction* action, const QString& id, const QString& context,
                               const QKeySequence& defaultKey) {
    auto& binding = m_bindings[id];
    if (binding.entry.id.isEmpty()) {
        binding.entry.id = id;
        binding.entry.context = context;
        binding.entry.text = action->text().remove(QLatin1Char('&'));
        binding.entry.defaultKey = defaultKey;
        const QSettings settings;
        const auto stored = settings.value(settingsKey(id));
        binding.entry.key = stored.isValid() ? QKeySequence(stored.toString(), QKeySequence::PortableText) : defaultKey;
    }
    binding.actions.push_back(action);
    apply(binding);
    return action;
}

QAction* ShortcutManager::create(QWidget* owner, const QString& id, const QString& context, const QString& text,
                                 const QKeySequence& defaultKey, std::function<void()> handler) {
    auto* action = new QAction(text, owner);
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(action, &QAction::triggered, owner, [handler = std::move(handler)] { handler(); });
    owner->addAction(action);
    return bind(action, id, context, defaultKey);
}

void ShortcutManager::apply(Binding& binding) {
    binding.actions.removeAll(nullptr);
    for (auto& action : binding.actions) {
        action->setShortcut(binding.entry.key);
    }
}

std::vector<ShortcutManager::Entry> ShortcutManager::entries() const {
    std::vector<Entry> out;
    out.reserve(m_bindings.size());
    for (const auto& [id, binding] : m_bindings) {
        out.push_back(binding.entry);
    }
    return out;
}

QKeySequence ShortcutManager::shortcut(const QString& id) const {
    const auto it = m_bindings.find(id);
    return it != m_bindings.end() ? it->second.entry.key : QKeySequence();
}

QKeySequence ShortcutManager::defaultShortcut(const QString& id) const {
    const auto it = m_bindings.find(id);
    return it != m_bindings.end() ? it->second.entry.defaultKey : QKeySequence();
}

void ShortcutManager::setShortcut(const QString& id, const QKeySequence& key) {
    const auto it = m_bindings.find(id);
    if (it == m_bindings.end()) {
        return;
    }
    it->second.entry.key = key;
    QSettings settings;
    if (key == it->second.entry.defaultKey) {
        settings.remove(settingsKey(id));
    } else {
        settings.setValue(settingsKey(id), key.toString(QKeySequence::PortableText));
    }
    apply(it->second);
    emit shortcutsChanged();
}

void ShortcutManager::resetAll() {
    QSettings settings;
    settings.remove(QStringLiteral("shortcuts"));
    for (auto& [id, binding] : m_bindings) {
        binding.entry.key = binding.entry.defaultKey;
        apply(binding);
    }
    emit shortcutsChanged();
}

std::vector<std::pair<QString, QString>> ShortcutManager::conflicts(const std::vector<Entry>& entries) {
    std::vector<std::pair<QString, QString>> out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        for (std::size_t j = i + 1; j < entries.size(); ++j) {
            const auto& a = entries[i];
            const auto& b = entries[j];
            if (a.key.isEmpty() || a.key != b.key) {
                continue;
            }
            const bool sameScope = a.context == b.context || a.context == QLatin1String(kApplicationContext) ||
                                   b.context == QLatin1String(kApplicationContext);
            if (sameScope) {
                out.emplace_back(a.id, b.id);
            }
        }
    }
    return out;
}

} // namespace loki::ui
