#include "shortcuts.h"

#include <QRegularExpression>
#include <qnamespace.h>

static const struct {
    const char* action;
    const char* defaultKey;
} k_ShortcutDefaults[] = {
    { "quit", "Q" },
    { "ungrabInput", "Z" },
    { "toggleFullScreen", "X" },
    { "toggleStats", "S" },
    { "toggleMouseMode", "M" },
    { "toggleCursorHide", "C" },
    { "minimize", "D" },
    { "pasteText", "V" },
    { "togglePointerRegionLock", "L" },
    { "quitAndExit", "E" },
    { "toggleKeyboardGrab", "K" },
};

QStringList ShortcutKeyNames::actions()
{
    QStringList actions;
    for (const auto& shortcut : k_ShortcutDefaults) {
        actions.append(shortcut.action);
    }
    return actions;
}

QString ShortcutKeyNames::defaultKey(const QString& action)
{
    for (const auto& shortcut : k_ShortcutDefaults) {
        if (action == shortcut.action) {
            return shortcut.defaultKey;
        }
    }
    return QString();
}

QString ShortcutKeyNames::fromQtKey(int qtKey)
{
    // Qt::Key values for letters and digits are their uppercase ASCII values
    if ((qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) || (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)) {
        return QString(QChar(qtKey));
    }
    else if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) {
        return QString("F%1").arg(qtKey - Qt::Key_F1 + 1);
    }

    // These must match SDL's scancode names
    switch (qtKey) {
    case Qt::Key_Space:
        return "Space";
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return "Tab";
    case Qt::Key_Backspace:
        return "Backspace";
    case Qt::Key_Delete:
        return "Delete";
    case Qt::Key_Insert:
        return "Insert";
    case Qt::Key_Home:
        return "Home";
    case Qt::Key_End:
        return "End";
    case Qt::Key_PageUp:
        return "PageUp";
    case Qt::Key_PageDown:
        return "PageDown";
    default:
        return QString();
    }
}

bool ShortcutKeyNames::isModifierQtKey(int qtKey)
{
    switch (qtKey) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
        return true;
    default:
        return false;
    }
}

QString ShortcutKeyNames::toDisplayText(int modifiers, const QString& keyName)
{
    if (keyName.isEmpty()) {
        return QString();
    }

    QStringList parts;
    if (modifiers & MOD_CTRL) {
        parts.append("Ctrl");
    }
    if (modifiers & MOD_ALT) {
#ifdef Q_OS_DARWIN
        parts.append("Option");
#else
        parts.append("Alt");
#endif
    }
    if (modifiers & MOD_SHIFT) {
        parts.append("Shift");
    }
    if (modifiers & MOD_GUI) {
#ifdef Q_OS_DARWIN
        parts.append("Cmd");
#else
        parts.append("Win");
#endif
    }
    parts.append(keyName);

    return parts.join('+');
}

QString ShortcutKeyNames::replaceDefaultShortcut(const QString& text, const QString& defaultKeyName,
                                                 int modifiers, const QString& keyName)
{
    if (modifiers == MOD_DEFAULT && keyName == defaultKeyName) {
        return text;
    }
    else if (keyName.isEmpty()) {
        return QString();
    }

    // Translations may rename the modifiers (e.g. "Strg+Alt+Shift+M"),
    // so match any three modifier names followed by the default key.
    QRegularExpression defaultShortcut(QString("[A-Za-z]+\\+[A-Za-z]+\\+[A-Za-z]+\\+%1(?![A-Za-z0-9])")
                                       .arg(QRegularExpression::escape(defaultKeyName)));
    return QString(text).replace(defaultShortcut, toDisplayText(modifiers, keyName));
}
