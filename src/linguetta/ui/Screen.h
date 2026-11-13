#pragma once
// Base for the Linguetta screens: render() registers tap targets in drawing order; loop()
// turns taps, Up/Down (and Left/Right, unless a screen claims them) and Confirm into
// onAction(id), and Back / the back swipe into onBack(). Targets are shared with the render
// task, so loop() reads them under the render lock.

#include <vector>

#include "Ui.h"
#include "activities/Activity.h"

namespace linguetta::ui {

class Screen : public Activity {
 public:
  using Activity::Activity;
  void loop() override;

 protected:
  virtual void onAction(int id) = 0;
  virtual void onBack() { finish(); }
  // Left / Right / page buttons. Return true when handled; otherwise they move the focus.
  virtual bool onSideButton(MappedInputManager::Button) { return false; }
  // Called every loop before input handling (polling screens).
  virtual void onTick() {}

  // ---- for render() ----
  void clearTargets() { targets.clear(); }
  Box target(Box b, int id, bool focusable = true) {
    targets.push_back({b, id, focusable});
    return b;
  }
  bool focused(int id) const { return focusId == id; }
  // Inset outline that marks the focused target (button navigation).
  void drawFocus(const Box& b) const;
  void setFocus(int id) { focusId = id; }
  int focus() const { return focusId; }

 private:
  struct Target {
    Box box;
    int id;
    bool focusable;
  };
  void moveFocus(int dir);
  std::vector<Target> targets;
  int focusId = -1;
};

}  // namespace linguetta::ui
