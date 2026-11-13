#include "Screen.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace linguetta::ui {

using Button = MappedInputManager::Button;

namespace {
constexpr fui::ActionId ITEM_ACTION = ACTION_ITEM;
constexpr fui::ActionId LIST_ACTION = 1;
}  // namespace

Screen::Screen(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity(name, renderer, mappedInput), uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {}

void Screen::onEnter() {
  Activity::onEnter();
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ITEM_ACTION, &Screen::onItem, this);
  app.on(LIST_ACTION, &Screen::onListRow, this);
  app.setScreen(&Screen::screenFn, this);
  focusId = -1;
  selected = 0;
  topIndex = 0;
  visibleRows = 1;
  fullRefresh = true;
}

const char* Screen::confirmHint() const { return tr(STR_SELECT); }

void Screen::onItem(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<Screen*>(user);
  self->tappedAction = event.value;
  self->app.clearTapFlash();
}

void Screen::onListRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<Screen*>(user);
  self->tappedRow = event.value;
  self->app.clearTapFlash();
}

void Screen::screenFn(Ui& ui, void* user) {
  auto* self = static_cast<Screen*>(user);
  if (self->hasChrome()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    ui.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding +
                                                         TouchHeaderBackButton::height(metrics, self->mappedInput) +
                                                         metrics.verticalSpacing),
                                    0, static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});
  }
  self->focusables.clear();
  self->rowSelectable.clear();
  self->build(ui);
}

// ---- widgets ----

fui::State Screen::focusable(const int id) {
  focusables.push_back(id);
  return id == focusId ? fui::StateFocused : fui::StateNormal;
}

int Screen::fontFor(const int role) const {
  const auto spec = uiScaleSpec();
  return role >= 2 ? spec.titleFontId : role == 1 ? spec.bodyFontId : spec.smallFontId;
}

int Screen::sidePadding() const { return UITheme::getInstance().getMetrics().contentSidePadding; }

fui::TextStyle Screen::style(Ui& ui, const int role, const bool bold, const fui::TextAlign align) const {
  fui::TextStyle ts = role >= 2 ? ui.theme().titleText : role == 1 ? ui.theme().bodyText : ui.theme().smallText;
  ts.bold = bold;
  ts.align = align;
  ts.maxLines = 1;
  return ts;
}

int Screen::paragraph(Ui& ui, const std::string& text, const int role, const bool bold, const fui::TextAlign align,
                      const int maxLines) {
  if (text.empty()) return 0;
  const int side = sidePadding();
  const int w = ui.body().width - 2 * side;
  const int font = fontFor(role);
  const auto lines = renderer.wrappedText(font, text.c_str(), w, maxLines,
                                          bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  const int lh = renderer.getLineHeight(font);
  const fui::TextStyle ts = style(ui, role, bold, align);
  int used = 0;
  for (const auto& line : lines) {
    const fui::Rect r = ui.takeTop(static_cast<int16_t>(lh));
    ui.target().text(fui::Rect{static_cast<int16_t>(r.x + side), r.y, static_cast<int16_t>(w), r.height},
                     line.c_str(), ts);
    used += lh;
  }
  return used;
}

void Screen::button(Ui& ui, fui::Rect rect, const char* label, const int id, const bool primary, const bool enabled) {
  fui::ButtonProps props;
  props.label = label;
  props.action = ITEM_ACTION;
  props.value = static_cast<int16_t>(id);
  props.state = enabled ? focusable(id) : fui::StateDisabled;
  props.text = style(ui, 1, primary, fui::TextAlign::Center);
  // Outlined like the option dialogs; the main action of a screen is the inverted one.
  props.styles = fui::outlinedButtonStyles(ui.theme().listRowRadius);
  if (primary) {
    for (fui::BoxStyle* b : {&props.styles.normal, &props.styles.selected, &props.styles.focused, &props.styles.active}) {
      b->background = fui::Paint::solid(fui::Color::Black);
      b->foreground = fui::Paint::solid(fui::Color::White);
    }
    props.styles.focused.border = fui::Paint::solid(fui::Color::Black);
    props.styles.focused.borderWidth = 3;
    props.text.color = fui::Color::White;
    props.styles.explicitlySet = true;
  } else {
    props.styles.focused.border = fui::Paint::solid(fui::Color::Black);
    props.styles.focused.borderWidth = 3;
    props.styles.focused.background = fui::Paint::solid(fui::Color::LightGray);
  }
  props.enabled = enabled;
  ui.button(props, rect);
}

void Screen::tapArea(Ui& ui, fui::Rect rect, const int id) {
  ui.frame().hit(rect, ITEM_ACTION, static_cast<int16_t>(id), fui::InputTouch);
}

fui::Rect Screen::bottomButton(Ui& ui, const char* label, const int id, const bool primary) {
  const auto& theme = ui.theme();
  const int side = sidePadding();
  fui::Rect r = ui.takeBottom(theme.rowHeight, theme.spaceMd);
  r = fui::Rect{static_cast<int16_t>(r.x + side), r.y, static_cast<int16_t>(r.width - 2 * side), r.height};
  button(ui, r, label, id, primary);
  return r;
}

void Screen::list(Ui& ui, std::vector<fui::ListItem>& items, const UiListRowType rowType, const bool boldLabels) {
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.action = LIST_ACTION;
  props.inputMask = fui::InputTouch;
  props.labelText = ui.theme().bodyText;
  props.labelText.bold = boldLabels;
  int firstSelectable = -1;
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].actionValue = static_cast<int16_t>(i);
    if (items[i].isHeader || !items[i].enabled) continue;
    rowSelectable.push_back(static_cast<int>(i));
    if (firstSelectable < 0) firstSelectable = static_cast<int>(i);
  }
  if (firstSelectable >= 0 && std::find(rowSelectable.begin(), rowSelectable.end(), selected) == rowSelectable.end()) {
    selected = firstSelectable;
  }
  props.selectedIndex = static_cast<int16_t>(firstSelectable >= 0 ? selected : -1);
  const auto rows = configureUiList(props, ui.theme(), ui.body(), rowType);
  visibleRows = rows > 0 ? rows : 1;
  const int count = static_cast<int>(items.size());
  topIndex = scrollListBy(topIndex, 0, visibleRows, count);
  if (followSelection && firstSelectable >= 0) {
    topIndex = followListSelection(selected, topIndex, visibleRows, count);
  }
  followSelection = false;
  props.topIndex = static_cast<uint16_t>(topIndex);
  ui.list(props);
}

// ---- render ----

void Screen::hints(const char*& back, const char*& confirm, const char*& up, const char*& down) const {
  back = tr(STR_BACK);
  confirm = confirmHint();
  up = tr(STR_DIR_UP);
  down = tr(STR_DIR_DOWN);
}

void Screen::render(RenderLock&&) {
  renderer.clearScreen();
  backdrop();
  if (hasChrome()) {
    const std::string t = title();
    const std::string sub = subtitle();
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    const char* subtitleText = sub.empty() ? nullptr : sub.c_str();
    if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::draw(renderer, uiTarget, header, t.c_str(), false, 0, subtitleText);
    } else {
      GUI.drawHeader(renderer, header, t.c_str(), subtitleText);
    }
  }
  uiReady = false;
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;
  if (hasChrome()) {
    const char *back = nullptr, *confirm = nullptr, *up = nullptr, *down = nullptr;
    hints(back, confirm, up, down);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(back ? back : ""), confirm, up, down);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(fullRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  fullRefresh = false;
}

// ---- input ----

void Screen::moveFocus(const int dir) {
  if (focusables.empty()) return;
  int at = -1;
  for (size_t i = 0; i < focusables.size(); i++) {
    if (focusables[i] == focusId) at = static_cast<int>(i);
  }
  const int n = static_cast<int>(focusables.size());
  at = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
  focusId = focusables[at];
  requestUpdate();
}

void Screen::moveSelection(const int dir) {
  if (rowSelectable.empty()) return;
  const int n = static_cast<int>(rowSelectable.size());
  int at = 0;
  for (int i = 0; i < n; i++) {
    if (rowSelectable[i] == selected) at = i;
  }
  at = (at + dir + n) % n;
  select(rowSelectable[at]);
  requestUpdate();
}

void Screen::loop() {
  onTick();
  if (hasChrome() && TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    onBack();
    return;
  }
  if (mappedInput.wasReleased(Button::Back)) {
    onBack();
    return;
  }

  // Touch: the FreeInkUI routing registered by the last render.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      tappedAction = tappedRow = -1;
      app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (tappedRow >= 0) {
        const int row = tappedRow;
        tappedRow = -1;
        select(row);
        onRow(row);
        return;
      }
      if (tappedAction >= 0) {
        const int id = tappedAction;
        tappedAction = -1;
        setFocus(id);
        onAction(id);
        return;
      }
    }
  }

  const int count = listCount();
  if (count > 0) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int next = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                                    visibleRows, count);
      if (next != topIndex) {
        topIndex = next;
        requestUpdate();
      }
      return;
    }
  }

  // Direction and page buttons: the screen first, then the default selection / focus movement.
  for (const Button b : {Button::Left, Button::Right, Button::Up, Button::Down, Button::PageBack, Button::PageForward}) {
    if (mappedInput.wasReleased(b) && onKey(b)) return;
  }
  if (count > 0) {
    const int page = std::max(1, visibleRows);
    buttonNavigator.onNextRelease([this] { moveSelection(+1); });
    buttonNavigator.onPreviousRelease([this] { moveSelection(-1); });
    buttonNavigator.onNextContinuous([this, page] {
      for (int i = 0; i < page; i++) moveSelection(+1);
    });
    buttonNavigator.onPreviousContinuous([this, page] {
      for (int i = 0; i < page; i++) moveSelection(-1);
    });
    if (mappedInput.wasReleased(Button::Confirm)) onRow(selected);
    return;
  }
  buttonNavigator.onNextRelease([this] { moveFocus(+1); });
  buttonNavigator.onPreviousRelease([this] { moveFocus(-1); });
  if (mappedInput.wasReleased(Button::Confirm)) {
    bool valid = false;
    {
      RenderLock lock(*this);  // the render task rebuilds `focusables`
      valid = std::find(focusables.begin(), focusables.end(), focusId) != focusables.end();
    }
    if (valid) onAction(focusId);
  }
}

}  // namespace linguetta::ui
