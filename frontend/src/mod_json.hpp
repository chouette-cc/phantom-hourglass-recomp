// Lightweight, dependency-free JSON parser and serializer for mod manifests (mod.json),
// per-mod configuration (config.json), and the Lua `ph.json` library.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ph::json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    static Value make_null() { return {}; }
    static Value make_bool(bool b) { Value v; v.type = Type::Bool; v.boolean = b; return v; }
    static Value make_number(double n) { Value v; v.type = Type::Number; v.number = n; return v; }
    static Value make_string(std::string s) { Value v; v.type = Type::String; v.str = std::move(s); return v; }
    static Value make_array() { Value v; v.type = Type::Array; return v; }
    static Value make_object() { Value v; v.type = Type::Object; return v; }

    const Value* get(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        const auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }

    std::string string_or(const std::string& key, const std::string& def = {}) const {
        const Value* v = get(key);
        return (v && v->is_string()) ? v->str : def;
    }

    bool bool_or(const std::string& key, bool def = false) const {
        const Value* v = get(key);
        return (v && v->is_bool()) ? v->boolean : def;
    }

    int64_t int_or(const std::string& key, int64_t def = 0) const {
        const Value* v = get(key);
        return (v && v->is_number()) ? int64_t(std::llround(v->number)) : def;
    }

    double number_or(const std::string& key, double def = 0.0) const {
        const Value* v = get(key);
        return (v && v->is_number()) ? v->number : def;
    }
};

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view src) : s_(src) {}

    bool parse(Value& out, std::string& err) {
        skip_ws();
        if (!parse_value(out, 0, err)) return false;
        skip_ws();
        if (pos_ != s_.size()) {
            err = "unexpected trailing characters at offset " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    void skip_ws() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                pos_++;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                // Allow // comments in mod.json for modder convenience.
                pos_ += 2;
                while (pos_ < s_.size() && s_[pos_] != '\n') pos_++;
            } else {
                break;
            }
        }
    }

    bool match(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    bool parse_value(Value& out, int depth, std::string& err) {
        if (depth > kMaxDepth) return err = "JSON nesting too deep", false;
        skip_ws();
        if (pos_ >= s_.size()) return err = "unexpected end of JSON", false;
        const char c = s_[pos_];
        if (c == 'n') {
            if (!match("null")) return err = "invalid literal", false;
            out = Value::make_null();
            return true;
        }
        if (c == 't') {
            if (!match("true")) return err = "invalid literal", false;
            out = Value::make_bool(true);
            return true;
        }
        if (c == 'f') {
            if (!match("false")) return err = "invalid literal", false;
            out = Value::make_bool(false);
            return true;
        }
        if (c == '"') {
            std::string str;
            if (!parse_string(str, err)) return false;
            out = Value::make_string(std::move(str));
            return true;
        }
        if (c == '[') return parse_array(out, depth + 1, err);
        if (c == '{') return parse_object(out, depth + 1, err);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out, err);
        err = std::string("unexpected character '") + c + "' at offset " + std::to_string(pos_);
        return false;
    }

    static void append_utf8(std::string& dst, uint32_t cp) {
        if (cp < 0x80) {
            dst.push_back(char(cp));
        } else if (cp < 0x800) {
            dst.push_back(char(0xC0 | (cp >> 6)));
            dst.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            dst.push_back(char(0xE0 | (cp >> 12)));
            dst.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            dst.push_back(char(0xF0 | (cp >> 18)));
            dst.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            dst.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    bool parse_string(std::string& out, std::string& err) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return err = "expected '\"'", false;
        pos_++;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return err = "unterminated escape", false;
                const char e = s_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!parse_hex4(cp)) return err = "invalid \\u escape", false;
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t lo = 0;
                            if (!parse_hex4(lo)) return err = "invalid surrogate escape", false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return err = "unknown escape sequence", false;
                }
            } else {
                out.push_back(c);
            }
        }
        return err = "unterminated string", false;
    }

    bool parse_number(Value& out, std::string& err) {
        const size_t start = pos_;
        if (s_[pos_] == '-') pos_++;
        if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return err = "invalid number", false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') pos_++;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            pos_++;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return err = "invalid fraction", false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') pos_++;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            pos_++;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) pos_++;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return err = "invalid exponent", false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') pos_++;
        }
        const std::string token(s_.substr(start, pos_ - start));
        char* end = nullptr;
        const double num = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(num)) return err = "number out of range", false;
        out = Value::make_number(num);
        return true;
    }

    bool parse_array(Value& out, int depth, std::string& err) {
        pos_++;  // '['
        out = Value::make_array();
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            pos_++;
            return true;
        }
        for (;;) {
            Value elem;
            if (!parse_value(elem, depth, err)) return false;
            out.arr.push_back(std::move(elem));
            skip_ws();
            if (pos_ >= s_.size()) return err = "unterminated array", false;
            if (s_[pos_] == ']') {
                pos_++;
                return true;
            }
            if (s_[pos_] != ',') return err = "expected ',' or ']'", false;
            pos_++;
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ']') {  // tolerant trailing comma
                pos_++;
                return true;
            }
        }
    }

    bool parse_object(Value& out, int depth, std::string& err) {
        pos_++;  // '{'
        out = Value::make_object();
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            pos_++;
            return true;
        }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key, err)) return false;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return err = "expected ':'", false;
            pos_++;
            Value val;
            if (!parse_value(val, depth, err)) return false;
            out.obj[std::move(key)] = std::move(val);
            skip_ws();
            if (pos_ >= s_.size()) return err = "unterminated object", false;
            if (s_[pos_] == '}') {
                pos_++;
                return true;
            }
            if (s_[pos_] != ',') return err = "expected ',' or '}'", false;
            pos_++;
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '}') {  // tolerant trailing comma
                pos_++;
                return true;
            }
        }
    }

    std::string_view s_;
    size_t pos_ = 0;
};

inline void escape_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(c));
                    out += buf;
                } else {
                    out.push_back(char(c));
                }
                break;
        }
    }
    out.push_back('"');
}

inline void write_indent(std::string& out, int indent) {
    out.push_back('\n');
    out.append(size_t(indent * 2), ' ');
}

inline void dump_impl(const Value& v, std::string& out, bool pretty, int indent) {
    switch (v.type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(v.number)) {
                out += "null";
                break;
            }
            const double rounded = std::trunc(v.number);
            if (v.number == rounded && std::fabs(v.number) < 9007199254740992.0) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%lld", (long long)rounded);
                out += buf;
            } else {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.12g", v.number);
                out += buf;
            }
            break;
        }
        case Type::String: escape_string(out, v.str); break;
        case Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i > 0) out.push_back(',');
                if (pretty) write_indent(out, indent + 1);
                dump_impl(v.arr[i], out, pretty, indent + 1);
            }
            if (pretty && !v.arr.empty()) write_indent(out, indent);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, val] : v.obj) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) write_indent(out, indent + 1);
                escape_string(out, k);
                out += pretty ? ": " : ":";
                dump_impl(val, out, pretty, indent + 1);
            }
            if (pretty && !v.obj.empty()) write_indent(out, indent);
            out.push_back('}');
            break;
        }
    }
}

}  // namespace detail

inline bool parse(std::string_view src, Value& out, std::string& error) {
    detail::Parser p(src);
    return p.parse(out, error);
}

inline std::string dump(const Value& v, bool pretty = true) {
    std::string out;
    detail::dump_impl(v, out, pretty, 0);
    if (pretty) out.push_back('\n');
    return out;
}

}  // namespace ph::json
