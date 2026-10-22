#include "json.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace sr2::json {

const Value& Value::null() {
    static const Value v;
    return v;
}

Value Value::Object() {
    Value v;
    v.type_ = Type::Object;
    return v;
}

Value Value::Array() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value& Value::set(std::string_view key, Value v) {
    if (!isObject()) *this = Object();
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(v);
            return items_[i];
        }
    }
    keys_.emplace_back(key);
    items_.push_back(std::move(v));
    return items_.back();
}

Value& Value::child(std::string_view key) {
    if (!isObject()) *this = Object();
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            if (!items_[i].isObject()) items_[i] = Object();
            return items_[i];
        }
    }
    return set(key, Object());
}

void Value::push(Value v) {
    if (!isArray()) *this = Array();
    items_.push_back(std::move(v));
}

const Value& Value::at(size_t i) const { return (isArray() && i < items_.size()) ? items_[i] : null(); }

const Value* Value::find(std::string_view key) const {
    if (!isObject()) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

const Value& Value::operator[](std::string_view key) const {
    const Value* v = find(key);
    return v ? *v : null();
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool run(Value& out, std::string& err) {
        // Skip UTF-8 BOM.
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF && static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF)
            p_ = 3;
        if (!value(out, 0)) return fail(err);
        ws();
        if (p_ != s_.size()) {
            msg_ = "trailing characters after document";
            return fail(err);
        }
        return true;
    }

private:
    std::string_view s_;
    size_t p_ = 0;
    std::string msg_;

    bool fail(std::string& err) {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < p_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        err = std::format("line {} col {}: {}", line, col, msg_.empty() ? "syntax error" : msg_);
        return false;
    }
    bool error(const char* m) {
        msg_ = m;
        return false;
    }
    bool eof() const { return p_ >= s_.size(); }
    char peek() const { return eof() ? '\0' : s_[p_]; }

    void ws() {
        while (!eof()) {
            char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++p_;
            } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '/') {
                while (!eof() && s_[p_] != '\n') ++p_;
            } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '*') {
                size_t e = s_.find("*/", p_ + 2);
                p_ = e == std::string_view::npos ? s_.size() : e + 2;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view word) {
        if (s_.substr(p_, word.size()) != word) return error("invalid literal");
        p_ += word.size();
        return true;
    }

    bool value(Value& v, int depth) {
        if (depth > 64) return error("nesting too deep");
        ws();
        switch (peek()) {
            case '{': return object(v, depth);
            case '[': return array(v, depth);
            case '"': v.type_ = Value::Type::String; return string(v.str_);
            case 't': v.type_ = Value::Type::Bool; v.bool_ = true; return literal("true");
            case 'f': v.type_ = Value::Type::Bool; v.bool_ = false; return literal("false");
            case 'n': v.type_ = Value::Type::Null; return literal("null");
            default: return number(v);
        }
    }

    bool number(Value& v) {
        size_t start = p_;
        if (peek() == '-') ++p_;
        while (!eof() && (isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.' || s_[p_] == 'e' ||
                          s_[p_] == 'E' || s_[p_] == '+' || s_[p_] == '-'))
            ++p_;
        if (p_ == start) return error("unexpected character");
        double d = 0;
        auto r = std::from_chars(s_.data() + start, s_.data() + p_, d);
        if (r.ec != std::errc() || r.ptr != s_.data() + p_) {
            p_ = start;
            return error("invalid number");
        }
        v.type_ = Value::Type::Number;
        v.num_ = d;
        return true;
    }

    static void appendUtf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) {
            o += static_cast<char>(cp);
        } else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& out) {
        if (p_ + 4 > s_.size()) return error("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[p_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return error("bad hex digit in \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        ++p_;  // opening quote
        out.clear();
        while (!eof()) {
            char c = s_[p_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (eof()) break;
            char e = s_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 1 < s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
                        p_ += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return error("invalid escape");
            }
        }
        return error("unterminated string");
    }

    bool array(Value& v, int depth) {
        v.type_ = Value::Type::Array;
        ++p_;
        for (;;) {
            ws();
            if (peek() == ']') { ++p_; return true; }
            Value item;
            if (!value(item, depth + 1)) return false;
            v.items_.push_back(std::move(item));
            ws();
            if (peek() == ',') { ++p_; continue; }
            if (peek() == ']') { ++p_; return true; }
            return error("expected ',' or ']'");
        }
    }

    bool object(Value& v, int depth) {
        v.type_ = Value::Type::Object;
        ++p_;
        for (;;) {
            ws();
            if (peek() == '}') { ++p_; return true; }
            if (peek() != '"') return error("expected string key");
            std::string key;
            if (!string(key)) return false;
            ws();
            if (peek() != ':') return error("expected ':'");
            ++p_;
            Value item;
            if (!value(item, depth + 1)) return false;
            v.keys_.push_back(std::move(key));
            v.items_.push_back(std::move(item));
            ws();
            if (peek() == ',') { ++p_; continue; }
            if (peek() == '}') { ++p_; return true; }
            return error("expected ',' or '}'");
        }
    }
};

bool Parse(std::string_view text, Value& out, std::string& error) {
    out = Value{};
    return Parser(text).run(out, error);
}

namespace {

void WriteString(std::string& o, std::string_view s) {
    o += '"';
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) o += std::format("\\u{:04x}", c);
                else o += ch;
        }
    }
    o += '"';
}

void WriteNumber(std::string& o, double d) {
    if (d != d || d == HUGE_VAL || d == -HUGE_VAL) {  // NaN / inf are not JSON
        o += "0";
        return;
    }
    if (d == static_cast<double>(static_cast<int64_t>(d)) && d > -1e15 && d < 1e15) {
        o += std::format("{}", static_cast<int64_t>(d));
        return;
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), d);  // shortest round-trip form
    o.append(buf, r.ptr);
}

void WriteValue(std::string& o, const Value& v, int indent) {
    auto nl = [&](int n) {
        o += '\n';
        o.append(static_cast<size_t>(n) * 2, ' ');
    };
    switch (v.type()) {
        case Value::Type::Null: o += "null"; break;
        case Value::Type::Bool: o += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: WriteNumber(o, v.asNumber()); break;
        case Value::Type::String: WriteString(o, v.asString()); break;
        case Value::Type::Array: {
            if (v.items().empty()) {
                o += "[]";
                break;
            }
            o += '[';
            for (size_t i = 0; i < v.items().size(); ++i) {
                if (i) o += ',';
                nl(indent + 1);
                WriteValue(o, v.items()[i], indent + 1);
            }
            nl(indent);
            o += ']';
            break;
        }
        case Value::Type::Object: {
            if (v.keys().empty()) {
                o += "{}";
                break;
            }
            o += '{';
            for (size_t i = 0; i < v.keys().size(); ++i) {
                if (i) o += ',';
                nl(indent + 1);
                WriteString(o, v.keys()[i]);
                o += ": ";
                WriteValue(o, v.items()[i], indent + 1);
            }
            nl(indent);
            o += '}';
            break;
        }
    }
}

}  // namespace

std::string Serialize(const Value& v) {
    std::string o;
    WriteValue(o, v, 0);
    o += '\n';
    return o;
}

}  // namespace sr2::json
