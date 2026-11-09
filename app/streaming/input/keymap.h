#pragma once

#include "SDL_compat.h"
#include "settings/streamingpreferences.h"

// Keyboard remapping between client and host OS conventions (streamfork spec 02).

// What each client modifier key (on either side) sends to the host
struct ModifierMap {
    StreamingPreferences::ModifierTarget gui; // Cmd on macOS, Windows key elsewhere
    StreamingPreferences::ModifierTarget ctrl;
    StreamingPreferences::ModifierTarget alt; // Option on macOS
};

struct MappedKey {
    SDL_Scancode scancode;
    Uint16 mod;
    bool drop; // The key must not be sent to the host
};

// Builds the map for the user's keyboard modifier mode:
// - Off: every modifier sends itself.
// - Mac to Windows: Cmd sends Ctrl, Ctrl sends the Windows key, Option sends Alt.
// - Auto: Mac to Windows on macOS clients, off elsewhere. We can't learn the host OS
//   from the host, so we assume it is Windows (it is for Apollo with virtual displays).
// - Custom: the modifierMap* preferences.
ModifierMap getModifierMap(const StreamingPreferences& prefs);

bool isIdentityModifierMap(const ModifierMap& map);

const char* modifierTargetName(StreamingPreferences::ModifierTarget target);

// Remaps a key event. The Windows key is only sent while system keys are captured,
// as with the original behavior; a modifier mapped to "none" is never sent.
MappedKey applyModifierMap(const ModifierMap& map, SDL_Scancode scancode, Uint16 mod, bool systemKeyCapture);

#ifndef QT_NO_DEBUG
// Asserts the expected behavior of applyModifierMap() in debug builds
void keymapSelfTest();
#endif
