#pragma once

// The edit model behind the focusable text field: a single-line, UTF-8-aware
// buffer with a cursor. It is pure and has NO SDL on its link line, so it lives
// in haiku_remote_core and is unit-tested directly; the SDL widget layer
// (src/ui/widgets.cpp) owns only the drawing and the keymap, and defers every
// edit to this model. Keeping the cursor arithmetic here is what stops a stray
// byte-vs-code-point bug from silently corrupting a hostname or key path typed
// into the profile editor.
//
// The cursor is a byte offset that is always kept on a UTF-8 code-point
// boundary. Horizontal motion and deletion step over whole code points, so a
// multi-byte character is never split.

#include <cstddef>
#include <string>
#include <string_view>

namespace haiku_remote {

class TextFieldModel {
public:
    TextFieldModel() = default;
    explicit TextFieldModel(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }
    [[nodiscard]] std::size_t cursor() const { return cursor_; }
    [[nodiscard]] bool empty() const { return text_.empty(); }

    // Replace the whole buffer; the cursor lands at the end.
    void set_text(std::string text);
    void clear();

    // Insert UTF-8 bytes at the cursor, advancing past them. Invalid trailing
    // bytes are inserted verbatim rather than dropped -- the model never silently
    // loses input.
    void insert(std::string_view utf8);

    // Delete the code point before / at the cursor. No-ops at the ends.
    void backspace();
    void del();

    // Move the cursor one code point left / right, or to an end.
    void move_left();
    void move_right();
    void move_home();
    void move_end();

private:
    std::string text_;
    std::size_t cursor_ = 0;
};

} // namespace haiku_remote
