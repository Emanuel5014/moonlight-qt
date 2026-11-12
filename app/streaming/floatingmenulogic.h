#pragma once

#include <vector>

// Pure logic of the floating stream menu (streamfork spec 08): geometry,
// value steps and the input state machine. No Qt or SDL here, so the
// self-test can be built and run on its own.

namespace FloatingMenuLogic {

// Window pixels, origin at the top left
struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const;
    bool operator==(const Rect& other) const;
};

// Sizes in window pixels (logical sizes multiplied by the window scale)
struct Metrics {
    int buttonSize;
    int rowHeight;
    int panelWidth;
    int padding;
    int cornerRadius;
    int dragThreshold;
};

Metrics scaledMetrics(float scale);

// The button is centered at (fracX * winW, fracY * winH), then moved
// to be fully inside the window (or at 0 when it doesn't fit).
Rect buttonRect(double fracX, double fracY, int winW, int winH, int size);

// Inverse of buttonRect(): button center relative to the window size.
// An empty window gives the default position (0.5, 0.0).
void buttonFraction(const Rect& button, int winW, int winH, double& fracX, double& fracY);

// Below the button when it fits, otherwise above, otherwise at the top.
Rect panelRect(const Rect& button, int rowCount, int winW, int winH, const Metrics& m);

// Row index under (x, y), or -1
int rowAt(const Rect& panel, int rowCount, const Metrics& m, int x, int y);

enum class ChoiceHalf { Previous, Next };
ChoiceHalf choiceHalfAt(const Rect& panel, int x);

struct Resolution {
    int w, h;
    bool operator==(const Resolution& other) const;
};

std::vector<Resolution> resolutionSteps(Resolution native, Resolution current);
std::vector<int> fpsSteps(int nativeHz, int current);
std::vector<int> bitrateStepsKbps(int currentKbps);

// Moves by delta from current inside steps, stopping at the ends.
// Returns current unchanged when it isn't in steps.
template<class T>
T stepValue(const std::vector<T>& steps, const T& current, int delta)
{
    for (int i = 0; i < (int)steps.size(); i++) {
        if (steps[i] == current) {
            int target = i + delta;
            if (target < 0) {
                target = 0;
            }
            else if (target >= (int)steps.size()) {
                target = (int)steps.size() - 1;
            }
            return steps[target];
        }
    }
    return current;
}

// Runs every check, prints failures to stderr and returns their count
int floatingMenuSelfTest();

}
