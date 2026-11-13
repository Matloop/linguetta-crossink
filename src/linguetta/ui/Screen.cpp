#include "Screen.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "activities/RenderLock.h"

namespace linguetta::ui {

using Button = MappedInputManager::Button;

void Screen::drawFocus(const Box& b) const {
  if (b.w <= 8 || b.h <= 8) return;
  renderer.drawRoundedRect(b.x - 4, b.y - 4, b.w + 8, b.h + 8, 2, 12, true);
}

void Screen::moveFocus(int dir) {
  std::vector<int> ids;
  for (const auto& t : targets) {
    if (t.focusable) ids.push_back(t.id);
  }
  if (ids.empty()) return;
  int at = -1;
  for (size_t i = 0; i < ids.size(); i++) {
    if (ids[i] == focusId) at = static_cast<int>(i);
  }
  const int n = static_cast<int>(ids.size());
  at = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
  focusId = ids[at];
  requestUpdate();
}

void Screen::loop() {
  onTick();
  if (mappedInput.wasReleased(Button::Back)  /* includes the touch back gesture */) {
    onBack();
    return;
  }
  int action = -2;
  {
    RenderLock lock(*this);
    int x = 0, y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      for (const auto& t : targets) {
        if (t.box.contains(x, y)) {
          action = t.id;
          break;
        }
      }
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      bool valid = false;
      for (const auto& t : targets) valid = valid || (t.focusable && t.id == focusId);
      if (valid) action = focusId;
    } else if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack)) {
      if (!onSideButton(Button::Up)) moveFocus(-1);
    } else if (mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward)) {
      if (!onSideButton(Button::Down)) moveFocus(+1);
    } else if (mappedInput.wasReleased(Button::Left)) {
      if (!onSideButton(Button::Left)) moveFocus(-1);
    } else if (mappedInput.wasReleased(Button::Right)) {
      if (!onSideButton(Button::Right)) moveFocus(+1);
    }
  }
  if (action != -2) onAction(action);
}

}  // namespace linguetta::ui
