#pragma once
// Tiny hand-written JSON reader/writer (RFC 8259 + // and /* */ comments +
// trailing commas tolerated on input). Enough for sr2mods.json. Writing does
// NOT preserve comments.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sr2::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? bool_ : def; }
    double asNumber(double def = 0) const { return isNumber() ? num_ : def; }
    int64_t asInt(int64_t def = 0) const { return isNumber() ? static_cast<int64_t>(num_) : def; }
    std::string asString(std::string_view def = {}) const { return isString() ? str_ : std::string(def); }

    // Arrays
    size_t size() const { return isArray() ? items_.size() : isObject() ? keys_.size() : 0; }
    const Value& at(size_t i) const;
    // Objects (nullptr / Null value if missing)
    const Value* find(std::string_view key) const;
    const Value& operator[](std::string_view key) const;
    const std::vector<std::string>& keys() const { return keys_; }
    const std::vector<Value>& items() const { return items_; }

    static const Value& null();

    // ---- construction / mutation (used by the settings writer) ----
    Value() = default;
    explicit Value(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Value(double d) : type_(Type::Number), num_(d) {}
    explicit Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    static Value Object();
    static Value Array();
    // Object: insert or replace `key` (turns a non-object into an empty object first).
    Value& set(std::string_view key, Value v);
    // Object: mutable child, created as an empty object if missing / not an object.
    Value& child(std::string_view key);
    void push(Value v);  // Array (turns a non-array into an empty array first)

private:
    friend class Parser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<std::string> keys_;  // object keys (parallel to items_)
    std::vector<Value> items_;       // array elements or object values
};

// Returns false and fills `error` ("line L col C: message") on failure.
bool Parse(std::string_view text, Value& out, std::string& error);

// Pretty-printed JSON (2-space indent, keys in insertion order, UTF-8).
std::string Serialize(const Value& v);

}  // namespace sr2::json
