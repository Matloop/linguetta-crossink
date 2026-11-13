#pragma once
// Base for the Linguetta screens, built the way CrossInk builds its own: a FreeInkUI app hosted by
// the activity, the shared theme tokens (fonts follow "uiScale", rows, buttons and colours follow
// the active UI theme), the touch header with the back button (the GUI header on button-only
// devices) and the GUI button hints.
//
// A screen supplies title(), build() (the body, between the header and the hints) and its actions.
// Two input models, as in the rest of the firmware:
//  - list screens (listCount() > 0): the direction buttons move the selection, swipes scroll,
//    Confirm and taps call onRow(row);
//  - button screens: build() calls button() / focusable() for each control, the direction buttons
//    move the focus among them and Confirm or a tap calls onAction(id).

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "Ui.h"
#include "activities/Activity.h"
#include "components/UIThemeTokens.h"
#include "util/ButtonNavigator.h"

namespace linguetta::ui {

namespace fui = freeink::ui;
using UiApp = fui::FreeInkApp<28, 2>;
using Ui = UiApp::ScreenType;

// The action FreeInkUI controls of button screens report (the control id is the value).
constexpr fui::ActionId ACTION_ITEM = 2;

class Screen : public Activity {
 public:
  Screen(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() final;
  void render(RenderLock&&) final;

 protected:
  // Header title; subtitle() goes under it (e.g. what is left to review).
  virtual std::string title() const = 0;
  virtual std::string subtitle() const { return ""; }
  // Body widgets. Called on the render task with the content margin already set.
  virtual void build(Ui& ui) = 0;
  virtual void onAction(int /*id*/) {}
  virtual void onRow(int /*row*/) {}
  virtual void onBack() { finish(); }
  // Called every loop before input handling (polling screens).
  virtual void onTick() {}
  // A direction / page button before the default handling. Return true when handled.
  virtual bool onKey(MappedInputManager::Button) { return false; }
  // List screens: number of rows of the list drawn by list().
  virtual int listCount() const { return 0; }
  // Label of the Confirm hint on button-only devices.
  virtual const char* confirmHint() const;
  // The four hints at the bottom of button-only devices (back, confirm, up, down).
  virtual void hints(const char*& back, const char*& confirm, const char*& up, const char*& down) const;
  // Header and hints are the screen's (the Blend footer is a sheet over the reader page).
  virtual bool hasChrome() const { return true; }
  // Drawn after the screen is cleared and before the widgets (the reader page under a sheet).
  virtual void backdrop() {}
  // Full panel refresh for the next render (once per screen change keeps ghosting down).
  void requestFullRefresh() { fullRefresh = true; }
  void useFastRefresh() { fullRefresh = false; }

  // ---- for build() ----
  // A control the direction buttons can focus; returns its state for the widget.
  fui::State focusable(int id);
  // Themed button at `rect`; a tap or Confirm (when focused) calls onAction(id).
  void button(Ui& ui, fui::Rect rect, const char* label, int id, bool primary = false, bool enabled = true);
  // A themed button in the bottom band of the body (the rows above keep their room).
  fui::Rect bottomButton(Ui& ui, const char* label, int id, bool primary = false);
  // The whole rect acts as a tap target for onAction(id) (touch only).
  void tapArea(Ui& ui, fui::Rect rect, int id);
  // Wrapped text at the top of the body, between the side paddings; returns the height used.
  // role: 0 small, 1 body, 2 title (the theme fonts).
  int paragraph(Ui& ui, const std::string& text, int role = 1, bool bold = false,
                fui::TextAlign align = fui::TextAlign::Left, int maxLines = 1);
  // The same text style the paragraph uses, for text drawn into a rect.
  fui::TextStyle style(Ui& ui, int role, bool bold = false, fui::TextAlign align = fui::TextAlign::Left) const;
  int fontFor(int role) const;  // GfxRenderer font id of a role
  int sidePadding() const;
  // Lays the list out in the rest of the body and keeps the viewport around the selection.
  // `items` (and the strings they point to) must outlive the call.
  void list(Ui& ui, std::vector<fui::ListItem>& items, UiListRowType rowType, bool boldLabels);

  int selected = 0;  // list screens: the selected row (an index into the items given to list())
  void setFocus(int id) {
    focusId = id;
    followSelection = true;
  }
  int focusedId() const { return focusId; }
  void select(int row) {
    selected = row;
    followSelection = true;
  }

 private:
  static void screenFn(Ui& ui, void* user);
  static void onItem(const fui::ActionEvent& event, void* user);
  static void onListRow(const fui::ActionEvent& event, void* user);
  void moveFocus(int dir);
  void moveSelection(int next);

  fui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  ButtonNavigator buttonNavigator;
  std::vector<int> focusables;
  std::vector<int> rowSelectable;  // list rows that can be selected, in order
  int focusId = -1;
  int tappedAction = -1, tappedRow = -1;
  int visibleRows = 1, topIndex = 0;
  bool followSelection = false;
  bool fullRefresh = true;
};

}  // namespace linguetta::ui
