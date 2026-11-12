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

enum class CommandKind { ActivateRow, StepRow, MoveFinished, Opened, Closed };

struct Command {
    CommandKind kind;
    int row;   // ActivateRow, StepRow
    int delta; // StepRow: -1 or +1
};

// Decides which mouse and Esc events belong to the menu (returning true
// means the event is consumed and must not reach the host) and turns
// them into commands. Coordinates are window pixels.
class MenuController
{
public:
    MenuController();

    void setWindow(int winW, int winH, const Metrics& m);
    void setPosition(double fracX, double fracY);
    void getPosition(double& fracX, double& fracY) const;
    // One entry per row: true for choice rows (split click, wheel)
    void setRows(const std::vector<bool>& isChoiceRow);
    void setButtonVisible(bool visible);

    bool isOpen() const;
    void open();
    void close();

    // Left mouse button
    bool mouseDown(int x, int y);
    bool mouseUp(int x, int y);
    // Any other mouse button (SDL button index): a press is consumed over
    // the menu, or anywhere while the panel is open (which closes it), and
    // never acted upon; a release is consumed exactly when its press was
    bool otherButtonDown(int button, int x, int y);
    bool otherButtonUp(int button, int x, int y);
    // A press the menu consumed hasn't been released yet
    bool hasPress() const;
    bool mouseMotion(int x, int y);
    bool mouseWheel(int x, int y, int dy);
    bool escDown();
    bool escUp();

    Rect button() const;
    Rect panel() const;
    int hoveredRow() const;
    bool isDragging() const;

    std::vector<Command> takeCommands();
    // True when something visible changed since the last call
    bool takeRedraw();

private:
    enum class Press { None, OnButton, OnPanel, OutsidePanel };

    bool overMenu(int x, int y) const;
    void setHovered(int row);

    int m_WinW, m_WinH;
    Metrics m_Metrics;
    double m_FracX, m_FracY;
    std::vector<bool> m_IsChoiceRow;
    bool m_ButtonVisible;
    bool m_Open;
    int m_Hovered;

    Press m_Press;
    int m_PressX, m_PressY;
    int m_PressRow;
    bool m_Dragging;
    Rect m_DragStart;
    Rect m_DragRect;
    bool m_SwallowEscUp;
    unsigned int m_OtherButtonsPressed; // Bit (button - 1) per consumed press

    std::vector<Command> m_Commands;
    bool m_Redraw;
};

// Runs every check, prints failures to stderr and returns their count
int floatingMenuSelfTest();

}
