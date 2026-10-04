#include "haiku_remote/text_field_model.hpp"

namespace haiku_remote {

namespace {

bool is_continuation(unsigned char byte)
{
    return (byte & 0xc0) == 0x80;
}

// The byte offset of the code point starting strictly before `offset`.
std::size_t prev_boundary(const std::string& text, std::size_t offset)
{
    if (offset == 0)
        return 0;
    std::size_t index = offset - 1;
    while (index > 0 && is_continuation(static_cast<unsigned char>(text[index])))
        --index;
    return index;
}

// The byte offset just past the code point starting at `offset`.
std::size_t next_boundary(const std::string& text, std::size_t offset)
{
    if (offset >= text.size())
        return text.size();
    std::size_t index = offset + 1;
    while (index < text.size()
           && is_continuation(static_cast<unsigned char>(text[index])))
        ++index;
    return index;
}

} // namespace

TextFieldModel::TextFieldModel(std::string text) { set_text(std::move(text)); }

void TextFieldModel::set_text(std::string text)
{
    text_ = std::move(text);
    cursor_ = text_.size();
}

void TextFieldModel::clear()
{
    text_.clear();
    cursor_ = 0;
}

void TextFieldModel::insert(std::string_view utf8)
{
    text_.insert(cursor_, utf8);
    cursor_ += utf8.size();
}

void TextFieldModel::backspace()
{
    if (cursor_ == 0)
        return;
    const std::size_t start = prev_boundary(text_, cursor_);
    text_.erase(start, cursor_ - start);
    cursor_ = start;
}

void TextFieldModel::del()
{
    if (cursor_ >= text_.size())
        return;
    const std::size_t end = next_boundary(text_, cursor_);
    text_.erase(cursor_, end - cursor_);
}

void TextFieldModel::move_left()
{
    cursor_ = prev_boundary(text_, cursor_);
}

void TextFieldModel::move_right()
{
    cursor_ = next_boundary(text_, cursor_);
}

void TextFieldModel::move_home() { cursor_ = 0; }

void TextFieldModel::move_end() { cursor_ = text_.size(); }

} // namespace haiku_remote
