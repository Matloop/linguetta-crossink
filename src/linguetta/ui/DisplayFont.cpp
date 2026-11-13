// Linguetta's own fonts, generated with lib/EpdFont/scripts/fontconvert.py ... --2bit --compress --pnum:
// - display: NotoSans Bold 28 pt, the lone word on a flashcard (UI fonts stop at 18 pt).
// - ipa: NotoSans Regular 12 pt plus --additional-intervals 0x0250,0x02FF (IPA, stress marks),
//   which the built-in UI fonts lack.
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <builtinFonts/linguetta_display_bold.h>
#include <builtinFonts/linguetta_ipa_regular.h>

#include "Ui.h"

namespace linguetta::ui {
namespace {
EpdFont displayBold(&linguetta_display_bold);
// Bold only: every style resolves to the same face.
EpdFontFamily displayFamily(&displayBold, &displayBold, &displayBold, &displayBold);
EpdFont ipaRegular(&linguetta_ipa_regular);
EpdFontFamily ipaFamily(&ipaRegular, &ipaRegular, &ipaRegular, &ipaRegular);
}  // namespace

void registerDisplayFont(GfxRenderer& r) {
  r.insertFont(DISPLAY_FONT_ID, displayFamily);
  r.insertFont(IPA_FONT_ID, ipaFamily);
}
int fontDisplay() { return DISPLAY_FONT_ID; }
int fontIpa() { return IPA_FONT_ID; }
}  // namespace linguetta::ui
