#pragma once

#include <QString>
#include <QStringList>

// Configurable special key combos (streamfork spec 01).
//
// Keys are stored as SDL scancode names (as returned by SDL_GetScancodeName()),
// which for the supported keys are identical to the SDL key names. An empty
// key name means the shortcut is disabled. This file has no SDL dependency so
// it can be used from the settings and the GUI code.
class ShortcutKeyNames
{
public:
    enum Modifier
    {
        MOD_CTRL = 0x1,
        MOD_ALT = 0x2,
        MOD_SHIFT = 0x4,
        MOD_GUI = 0x8,

        MOD_ALL = MOD_CTRL | MOD_ALT | MOD_SHIFT | MOD_GUI,
        MOD_DEFAULT = MOD_CTRL | MOD_ALT | MOD_SHIFT
    };

    // Stable action identifiers, also used as QSettings keys
    static QStringList actions();

    static QString defaultKey(const QString& action);

    // Returns the key name for a Qt::Key value, or an empty string
    // if the key is not allowed in a shortcut.
    static QString fromQtKey(int qtKey);

    // Returns true if the Qt::Key value is a modifier key
    static bool isModifierQtKey(int qtKey);

    // Human-readable shortcut text like "Ctrl+Alt+Shift+Q".
    // Returns an empty string if the key name is empty (disabled).
    static QString toDisplayText(int modifiers, const QString& keyName);

    // Replaces the default shortcut (like "Ctrl+Alt+Shift+M") in a possibly translated
    // UI string with the configured one. The text is returned unchanged if the shortcut
    // has its default value, and an empty string is returned if the shortcut is disabled.
    static QString replaceDefaultShortcut(const QString& text, const QString& defaultKeyName,
                                          int modifiers, const QString& keyName);
};
