// Speech-bubble widget (issue #31, step 1 of 6 of the UI restructure in #29;
// wrapped in the Bubble class in step 2 of 11 of the refactoring plan in
// #42, issue #45).
//
// Provides the geometry, text table, scrolling and rendering for the bubble
// that hosts ALL content (prompt, response, the idle "hello"). Pure
// 1:1 wrap of the former free functions - behavior is unchanged.
//
//   bubble.setText(text) - word-wrap into the static 64-line table (15
//                          chars/line), reset the scroll offset to 0
//   bubble.scroll(down)  - move the offset by 1 line, clamped to
//                          [0, max(0, count - BUBBLE_VISIBLE_LINES)]
//   bubble.clear()       - empty table, offset 0
//   bubble.lineCount()   - number of wrapped lines in use (the "Response
//                          x/y" status counter, step 4)
//   bubble.scrollOffset()- index of the first visible line
//   bubble.render()      - draw the bubble rectangle (always, even when
//                          empty) + tail triangle + the visible 5-line
//                          window into the current frame
//   bubble.renderText(t) - draw the bubble frame + up to 5 lines of `t`
//                          (or an empty bubble if NULL) WITHOUT touching
//                          the line table (the stored content survives
//                          the call - the idle animation uses it, issue
//                          #35 step 5 follow-up)
//
// The bubble is always the same size and always drawn (even when empty) -
// no popping rectangle (issue #29 Q8). A small ~4 px tail triangle on its
// left edge points toward the alien (issue #29 Q9, cosmetic).
//
// The bubble owns its own static line table (BUBBLE_MAX_LINES x 16 B =
// 1 KB of static RAM) - no globals in the sketch. The panel is a
// constructor-injected reference (issue #52, step 9 of 11 of the
// refactoring plan in #42: the `extern Adafruit_SSD1306` in bubble.cpp is
// gone - the same pattern as the Display class, issue #51, step 8).
//
// bubble.render() does NOT clearDisplay() or display() on its own - the
// single render pass (one clearDisplay() + one display() per frame) is
// issued by displayMgr.render() (display.cpp). It may be called after the
// caller has drawn the rest of the scene.
#pragma once

#include "config.h"  // BUBBLE_* geometry + table size

class String;  // forward declaration (complete type via <Arduino.h> in the .cpp)
class Adafruit_SSD1306;  // forward declaration (complete type via
// <Adafruit_SSD1306.h> in bubble.cpp - the same pattern as display.h,
// finding #7 in the #42 audit)

// The speech-bubble widget: word-wrapped text table + scrolling + rendering
// (see the header note for the API). The state (lines_ / count_ / offset_)
// is private; the line table is a single static 1 KB definition shared by
// all instances (there is one - the shared `bubble` object).
class Bubble {
 public:
  // issue #52, step 9 of 11 of the refactoring plan in #42: the panel is
  // a constructor-injected reference (the `extern Adafruit_SSD1306` in
  // bubble.cpp is gone - the same pattern as the Display class, issue
  // #51, step 8). The panel is sketch-lifetime (a member of the shared
  // `hw` object, hardware.cpp), so the reference is valid for the whole
  // program. Defined in bubble.cpp, where the type is complete.
  explicit Bubble(Adafruit_SSD1306& panel);

  // Word-wrap `text` into the static 64-line table at
  // BUBBLE_CHARS_PER_LINE chars/line (via the width-parameterized
  // wrapText()) and reset the scroll offset to 0. Lines beyond
  // BUBBLE_MAX_LINES are dropped (same silent-truncation behavior as the
  // response view before the wrap).
  void setText(const String& text);

  // Move the scroll offset by 1 line (down = +1, up = -1), clamped to
  // [0, max(0, count - BUBBLE_VISIBLE_LINES)].
  void scroll(bool down);

  // Empty the table and reset the scroll offset to 0.
  void clear();

  // Number of wrapped lines actually in use (for the "Response x/y" status
  // counter, step 4).
  int lineCount() const;

  // Index of the first visible line (for the "Response x/y" status counter,
  // step 4).
  int scrollOffset() const;


  // Draw the bubble rectangle (always, even when empty) + the tail triangle
  // + the visible BUBBLE_VISIBLE_LINES window of the table into the current
  // frame. No clearDisplay() / display() of its own (see the header note).
  void render();

  // Draw the bubble frame (rectangle + tail) + up to BUBBLE_VISIBLE_LINES
  // lines of `text` (word-wrapped at BUBBLE_CHARS_PER_LINE, first lines
  // shown) into the current frame WITHOUT touching the line table
  // (lines_ / count_ / offset_) - the stored content (e.g. the response)
  // survives the call. text = NULL draws an empty bubble. Used by the
  // idle-alien animation (alien.renderBubble()) so the animation never
  // destroys the response text (issue #35 step 5 follow-up, Q4). No
  // clearDisplay() / display() of its own (see the header note).
  void renderText(const char* text);

 private:
  // The panel (constructor-injected reference, issue #52, step 9).
  Adafruit_SSD1306& panel_;

  // The static line table (BUBBLE_MAX_LINES x 16 B = 1 KB of static RAM,
  // issue #29 Q7) - one definition, shared by all instances.
  static char lines_[BUBBLE_MAX_LINES][BUBBLE_CHARS_PER_LINE + 1];
  int count_ = 0;    // wrapped lines actually in use
  int offset_ = 0;   // index of the first visible line

  // Draw the bubble frame (rectangle + interior clear + tail) + up to
  // BUBBLE_VISIBLE_LINES lines of `lines` (the first `count` are valid)
  // into the current frame. Shared by render() (the stored table window)
  // and renderText() (a transient text, table untouched). `lines` is a
  // pointer to the first row of a [..][BUBBLE_CHARS_PER_LINE + 1] table.
  // (A member function since issue #52, step 9: it draws on the
  // constructor-injected panel.)
  void drawFrame(char (*lines)[BUBBLE_CHARS_PER_LINE + 1], int count);
};

// The shared bubble object (the codebase's existing shared-object pattern;
// the instance is defined in TamAIgotchi.ino next to the other shared
// objects, the same pattern as the shared `display` object).
extern Bubble bubble;
