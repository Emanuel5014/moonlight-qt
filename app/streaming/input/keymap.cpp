#include "keymap.h"

static const ModifierMap k_IdentityMap = {
    StreamingPreferences::MT_WIN,
    StreamingPreferences::MT_CTRL,
    StreamingPreferences::MT_ALT,
};

static const ModifierMap k_MacToWindowsMap = {
    StreamingPreferences::MT_CTRL,
    StreamingPreferences::MT_WIN,
    StreamingPreferences::MT_ALT,
};

ModifierMap getModifierMap(const StreamingPreferences& prefs)
{
    switch (prefs.keyboardModifierMode) {
    case StreamingPreferences::KMM_MAC_TO_WINDOWS:
        return k_MacToWindowsMap;
    case StreamingPreferences::KMM_AUTO:
#ifdef Q_OS_DARWIN
        return k_MacToWindowsMap;
#else
        return k_IdentityMap;
#endif
    case StreamingPreferences::KMM_CUSTOM:
        return { prefs.modifierMapGui, prefs.modifierMapCtrl, prefs.modifierMapAlt };
    case StreamingPreferences::KMM_OFF:
    default:
        return k_IdentityMap;
    }
}

bool isIdentityModifierMap(const ModifierMap& map)
{
    return map.gui == k_IdentityMap.gui && map.ctrl == k_IdentityMap.ctrl && map.alt == k_IdentityMap.alt;
}

const char* modifierTargetName(StreamingPreferences::ModifierTarget target)
{
    switch (target) {
    case StreamingPreferences::MT_CTRL:
        return "Ctrl";
    case StreamingPreferences::MT_WIN:
        return "Win";
    case StreamingPreferences::MT_ALT:
        return "Alt";
    case StreamingPreferences::MT_NONE:
    default:
        return "none";
    }
}

static int targetModBit(StreamingPreferences::ModifierTarget target, bool right)
{
    switch (target) {
    case StreamingPreferences::MT_CTRL:
        return right ? KMOD_RCTRL : KMOD_LCTRL;
    case StreamingPreferences::MT_WIN:
        return right ? KMOD_RGUI : KMOD_LGUI;
    case StreamingPreferences::MT_ALT:
        return right ? KMOD_RALT : KMOD_LALT;
    case StreamingPreferences::MT_NONE:
    default:
        return 0;
    }
}

static SDL_Scancode targetScancode(StreamingPreferences::ModifierTarget target, bool right)
{
    switch (target) {
    case StreamingPreferences::MT_CTRL:
        return right ? SDL_SCANCODE_RCTRL : SDL_SCANCODE_LCTRL;
    case StreamingPreferences::MT_WIN:
        return right ? SDL_SCANCODE_RGUI : SDL_SCANCODE_LGUI;
    case StreamingPreferences::MT_ALT:
        return right ? SDL_SCANCODE_RALT : SDL_SCANCODE_LALT;
    case StreamingPreferences::MT_NONE:
    default:
        return SDL_SCANCODE_UNKNOWN;
    }
}

MappedKey applyModifierMap(const ModifierMap& map, SDL_Scancode scancode, Uint16 mod, bool systemKeyCapture)
{
    MappedKey mapped = { scancode, mod, false };

    if (isIdentityModifierMap(map)) {
        return mapped;
    }

    // Rebuild the modifier flags from the mapped targets
    int newMod = mod & ~(KMOD_CTRL | KMOD_ALT | KMOD_GUI);
    const struct {
        int bit;
        StreamingPreferences::ModifierTarget target;
        bool right;
    } modBits[] = {
        { KMOD_LGUI, map.gui, false },
        { KMOD_RGUI, map.gui, true },
        { KMOD_LCTRL, map.ctrl, false },
        { KMOD_RCTRL, map.ctrl, true },
        { KMOD_LALT, map.alt, false },
        { KMOD_RALT, map.alt, true },
    };
    for (const auto& modBit : modBits) {
        if (mod & modBit.bit) {
            newMod |= targetModBit(modBit.target, modBit.right);
        }
    }
    mapped.mod = (Uint16)newMod;

    // Remap the modifier keys themselves
    StreamingPreferences::ModifierTarget target;
    bool right;
    switch (scancode) {
    case SDL_SCANCODE_LGUI:
    case SDL_SCANCODE_RGUI:
        target = map.gui;
        right = scancode == SDL_SCANCODE_RGUI;
        break;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        target = map.ctrl;
        right = scancode == SDL_SCANCODE_RCTRL;
        break;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:
        target = map.alt;
        right = scancode == SDL_SCANCODE_RALT;
        break;
    default:
        return mapped;
    }

    mapped.scancode = targetScancode(target, right);
    mapped.drop = target == StreamingPreferences::MT_NONE ||
                  (target == StreamingPreferences::MT_WIN && !systemKeyCapture);
    return mapped;
}

#ifndef QT_NO_DEBUG
void keymapSelfTest()
{
    const ModifierMap off = k_IdentityMap;
    const ModifierMap mac = k_MacToWindowsMap;
    const ModifierMap custom = { StreamingPreferences::MT_CTRL, StreamingPreferences::MT_NONE, StreamingPreferences::MT_WIN };
    MappedKey mapped;

    // Off leaves everything untouched
    mapped = applyModifierMap(off, SDL_SCANCODE_LGUI, KMOD_LGUI, false);
    SDL_assert(mapped.scancode == SDL_SCANCODE_LGUI && mapped.mod == KMOD_LGUI && !mapped.drop);

    // Mac to Windows: Cmd+C becomes Ctrl+C
    mapped = applyModifierMap(mac, SDL_SCANCODE_C, KMOD_LGUI, false);
    SDL_assert(mapped.scancode == SDL_SCANCODE_C && mapped.mod == KMOD_LCTRL && !mapped.drop);

    // The Cmd keys become the Ctrl keys, even without system key capture
    mapped = applyModifierMap(mac, SDL_SCANCODE_RGUI, KMOD_RGUI, false);
    SDL_assert(mapped.scancode == SDL_SCANCODE_RCTRL && mapped.mod == KMOD_RCTRL && !mapped.drop);

    // The Ctrl keys become the Windows keys, only sent with system key capture
    mapped = applyModifierMap(mac, SDL_SCANCODE_LCTRL, KMOD_LCTRL, false);
    SDL_assert(mapped.scancode == SDL_SCANCODE_LGUI && mapped.mod == KMOD_LGUI && mapped.drop);
    mapped = applyModifierMap(mac, SDL_SCANCODE_LCTRL, KMOD_LCTRL, true);
    SDL_assert(mapped.scancode == SDL_SCANCODE_LGUI && !mapped.drop);

    // Option, Shift and lock keys are untouched
    mapped = applyModifierMap(mac, SDL_SCANCODE_Z, (Uint16)(KMOD_LALT | KMOD_SHIFT | KMOD_CAPS | KMOD_LGUI), false);
    SDL_assert(mapped.mod == (KMOD_LALT | KMOD_SHIFT | KMOD_CAPS | KMOD_LCTRL));

    // Custom: Ctrl is never sent, Option sends the Windows key
    mapped = applyModifierMap(custom, SDL_SCANCODE_RCTRL, KMOD_RCTRL, true);
    SDL_assert(mapped.drop && mapped.mod == 0);
    mapped = applyModifierMap(custom, SDL_SCANCODE_LALT, KMOD_LALT, true);
    SDL_assert(mapped.scancode == SDL_SCANCODE_LGUI && mapped.mod == KMOD_LGUI && !mapped.drop);
    mapped = applyModifierMap(custom, SDL_SCANCODE_X, (Uint16)(KMOD_LCTRL | KMOD_RGUI), false);
    SDL_assert(mapped.scancode == SDL_SCANCODE_X && mapped.mod == KMOD_RCTRL);

    (void)mapped;
}
#endif
