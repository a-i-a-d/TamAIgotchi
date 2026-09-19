// Speech-bubble widget (issue #31, step 1 of 6 of the UI restructure in #29;
// wrapped in the Bubble class in step 2 of 11 of the refactoring plan in
// #42, issue #45 - 1:1 wrap, behavior unchanged).
#include "bubble.h"
#include <Arduino.h>  // String, Serial, F()
#include <Adafruit_SSD1306.h>  // for the shared `display` object
#include "text_utils.h"  // wrapText() (the new width-parameterized core)

// issue #52, step 9 of 11 of the refactoring plan in #42: the `extern
// Adafruit_SSD1306 display;` is gone - the panel is a constructor-injected
// reference (the same pattern as the Display class, issue #51, step 8).

// The bubble owns its own static line table (BUBBLE_MAX_LINES x 16 B =
// 1 KB of static RAM, issue #29 Q7) - no globals in the sketch. The state
// variables are named count_ / offset_ (the accessors are lineCount() /
// scrollOffset()). One definition, shared by all instances (there is one:
// the shared `bubble` object).
char Bubble::lines_[BUBBLE_MAX_LINES][BUBBLE_CHARS_PER_LINE + 1];

// issue #52, step 9 of 11 of the refactoring plan in #42: the panel is
// injected by reference - the extern above is gone. The panel is
// sketch-lifetime (a member of the shared `hw` object, hardware.cpp), so
// the reference is valid for the whole program.
Bubble::Bubble(Adafruit_SSD1306& panel) : panel_(panel) {}

// Word-wrap `text` into the static table at BUBBLE_CHARS_PER_LINE
// chars/line and reset the scroll offset to 0.
void Bubble::setText(const String& text) {
  char* out[BUBBLE_MAX_LINES];
  for (int i = 0; i < BUBBLE_MAX_LINES; i++) out[i] = lines_[i];
  count_ = wrapText(text, out, BUBBLE_CHARS_PER_LINE, BUBBLE_MAX_LINES);
  offset_ = 0;
}

// Move the scroll offset by 1 line (down = +1, up = -1), clamped to
// [0, max(0, count - BUBBLE_VISIBLE_LINES)].
void Bubble::scroll(bool down) {
  int maxOffset = (count_ > BUBBLE_VISIBLE_LINES)
                ? count_ - BUBBLE_VISIBLE_LINES
                : 0;
  if (down) {
    if (offset_ < maxOffset) offset_++;
  } else {
    if (offset_ > 0) offset_--;
  }
}

// Empty the table and reset the scroll offset to 0.
void Bubble::clear() {
  for (int i = 0; i < BUBBLE_MAX_LINES; i++) lines_[i][0] = '\0';
  count_ = 0;
  offset_ = 0;
}

int Bubble::lineCount() const { return count_; }
int Bubble::scrollOffset() const { return offset_; }


// Draw the bubble frame (rectangle + interior clear + tail) + up to
// BUBBLE_VISIBLE_LINES lines of `lines[]` (the first `count` are valid)
// into the current frame. Shared by render() (the stored table window)
// and renderText() (a transient text, table untouched).
// Text is padded 3 px in from the left edge and starts 3 px below the top
// edge (font 1 = 6x8 px/char).
void Bubble::drawFrame(char (*lines)[BUBBLE_CHARS_PER_LINE + 1], int count) {
  // The bubble rectangle - always the same size, always drawn (issue #29 Q8).
  panel_.drawRect(BUBBLE_X, BUBBLE_Y, BUBBLE_W, BUBBLE_H, WHITE);

  // Clear the interior before re-drawing the visible window: the bubble is
  // re-rendered in place on every scroll / jump (issue #34, step 4), so a
  // jump back up must not leave stale text from a longer window behind.
  panel_.fillRect(BUBBLE_X + 1, BUBBLE_Y + 1, BUBBLE_W - 2, BUBBLE_H - 2, BLACK);

  // Tail: a small ~4 px filled triangle from the bubble's left edge
  // (x = BUBBLE_X = 30) toward the alien (x 4..27), vertically centered-ish
  // (issue #29 Q9, cosmetic - a 1-line revert if it looks off on the panel).
  panel_.fillTriangle(BUBBLE_X - 3, BUBBLE_Y + 18,   // tip (27, 36)
                      BUBBLE_X,     BUBBLE_Y + 14,   // base top (30, 32)
                      BUBBLE_X,     BUBBLE_Y + 22,   // base bottom (30, 40)
                      WHITE);

  if (count > BUBBLE_VISIBLE_LINES) count = BUBBLE_VISIBLE_LINES;
  for (int i = 0; i < count; i++) {
    panel_.setCursor(BUBBLE_X + 3, BUBBLE_Y + 3 + 8 * i);
    panel_.print(lines[i]);
  }
}

// Draw the bubble rectangle (always, even when empty) + the tail triangle
// + the visible BUBBLE_VISIBLE_LINES window of the table into the current
// frame. No clearDisplay() / display() of its own (the single render pass
// is displayMgr.render() in display.cpp; it issues display.display()).
void Bubble::render() {
  // The visible window of the table (BUBBLE_VISIBLE_LINES lines), starting
  // at scrollOffset.
  int maxOffset = (count_ > BUBBLE_VISIBLE_LINES)
                ? count_ - BUBBLE_VISIBLE_LINES
                : 0;
  if (offset_ < 0) offset_ = 0;
  if (offset_ > maxOffset) offset_ = maxOffset;

  drawFrame(&lines_[offset_], count_ - offset_);
}

// Draw the bubble frame + up to BUBBLE_VISIBLE_LINES lines of `text`
// (word-wrapped at BUBBLE_CHARS_PER_LINE, first lines shown) into the
// current frame WITHOUT touching the line table (lines_ / count_ /
// offset_) - the stored content (e.g. the response) survives the call
// (issue #35 step 5 follow-up, Q4: the idle animation must not destroy
// the response text). text = NULL draws an empty bubble.
// No clearDisplay() / display() of its own (see the header note).
void Bubble::renderText(const char* text) {
  if (text == NULL) {
    drawFrame(&lines_[0], 0);  // valid pointer, zero lines
    return;
  }
  char lines[BUBBLE_VISIBLE_LINES][BUBBLE_CHARS_PER_LINE + 1];
  char* out[BUBBLE_VISIBLE_LINES];
  for (int i = 0; i < BUBBLE_VISIBLE_LINES; i++) out[i] = lines[i];
  int count = wrapText(String(text), out, BUBBLE_CHARS_PER_LINE, BUBBLE_VISIBLE_LINES);
  drawFrame(&lines[0], count);
}
