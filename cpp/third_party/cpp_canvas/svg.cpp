#include "svg.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// An SVG document is parsed once into a retained tree of Nodes carrying baked
// Path2D geometry, resolved (inherited) presentation state and pre-evaluated
// SMIL timelines. render() walks that tree emitting Canvas commands only; it
// never touches the source text and allocates nothing per frame.
// ---------------------------------------------------------------------------
namespace svgc {

constexpr float kPi = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;
using Mat = std::array<float, 6>;
constexpr Mat kUnit{1, 0, 0, 1, 0, 0};

Mat concat(const Mat& a, const Mat& b) {
    return Mat{a[0]*b[0] + a[2]*b[1], a[1]*b[0] + a[3]*b[1],
               a[0]*b[2] + a[2]*b[3], a[1]*b[2] + a[3]*b[3],
               a[0]*b[4] + a[2]*b[5] + a[4], a[1]*b[4] + a[3]*b[5] + a[5]};
}
Mat shift(float x, float y) { return Mat{1, 0, 0, 1, x, y}; }

// --- text -------------------------------------------------------------------
bool space(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
std::string trim(const std::string& s) {
    size_t a=0, b=s.size();
    while (a<b && space(s[a])) ++a;
    while (b>a && space(s[b-1])) --b;
    return s.substr(a, b-a);
}
std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
bool hasWord(const std::string& list, const std::string& word) {
    size_t i=0;
    while (i < list.size()) {
        while (i<list.size() && space(list[i])) ++i;
        const size_t start=i;
        while (i<list.size() && !space(list[i])) ++i;
        if (i>start && list.compare(start, i-start, word)==0) return true;
    }
    return false;
}
void split(const std::string& s, char sep, std::vector<std::string>& out) {
    size_t i=0;
    for (;;) {
        const size_t at = s.find(sep, i);
        out.push_back(trim(s.substr(i, (at==std::string::npos ? s.size() : at) - i)));
        if (at==std::string::npos) break;
        i = at+1;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
}

// SVG numbers: sign, digits, one dot, optional exponent. strtof consumes the
// longest valid prefix, which is exactly what the ".5.5" compact form needs.
bool scanNumber(const char*& p, const char* end, float& value) {
    while (p<end && (space(*p) || *p==',')) ++p;
    if (p>=end) return false;
    const unsigned char c = static_cast<unsigned char>(*p);
    if (!(std::isdigit(c) || c=='.' || c=='+' || c=='-')) return false;
    char* stop = nullptr;
    const float parsed = std::strtof(p, &stop);
    if (!stop || stop==p) return false;
    if (stop > end) stop = const_cast<char*>(end);
    p = stop;
    value = std::isfinite(parsed) ? parsed : 0.f;
    return true;
}
bool scanFlag(const char*& p, const char* end, bool& flag) {
    while (p<end && (space(*p) || *p==',')) ++p;
    if (p>=end || (*p!='0' && *p!='1')) return false;
    flag = *p=='1'; ++p; return true;
}
void numbersInto(const std::string& s, std::vector<float>& out) {
    const char* p = s.c_str(); const char* end = p + s.size(); float v;
    while (p < end) { const char* was=p; if (scanNumber(p,end,v)) out.push_back(v); if (p==was) ++p; }
}
std::vector<float> numbers(const std::string& s) { std::vector<float> out; numbersInto(s, out); return out; }
float scalar(const std::string& s, float fallback = 0) {
    const char* p = s.c_str(); const char* end = p + s.size(); float v;
    while (p < end) { const char* was=p; if (scanNumber(p,end,v)) return v; if (p==was) ++p; }
    return fallback;
}
float seconds(const std::string& s) {
    const float v = scalar(s, 0);
    return s.find("ms") != std::string::npos ? v/1000.f : s.find("min") != std::string::npos ? v*60.f : v;
}

// --- colour -----------------------------------------------------------------
struct Named { const char* name; unsigned rgb; };
// Only the names the artwork actually uses, plus the obvious neighbours.
const Named kNamed[] = {
    {"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"lime",0x00ff00},{"green",0x008000},
    {"blue",0x0000ff},{"yellow",0xffff00},{"cyan",0x00ffff},{"aqua",0x00ffff},{"magenta",0xff00ff},
    {"fuchsia",0xff00ff},{"gray",0x808080},{"grey",0x808080},{"silver",0xc0c0c0},{"maroon",0x800000},
    {"olive",0x808000},{"navy",0x000080},{"teal",0x008080},{"purple",0x800080},{"orange",0xffa500},
    {"gold",0xffd700},{"pink",0xffc0cb},{"brown",0xa52a2a},{"tan",0xd2b48c},{"beige",0xf5f5dc},
    {"ivory",0xfffff0},{"darkgray",0xa9a9a9},{"darkgrey",0xa9a9a9},{"lightgray",0xd3d3d3},{"lightgrey",0xd3d3d3},
};
int hexDigit(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
enum : unsigned char { PaintNone, PaintColor, PaintUnknown };
unsigned char parsePaint(const std::string& raw, Color& out) {
    const std::string v = lower(trim(raw));
    if (v.empty() || v=="none" || v=="transparent") return PaintNone;
    if (v[0]=='#') {
        int d[8]; size_t n=0; bool ok=true;
        for (size_t i=1; i<v.size(); ++i) {
            const int h = hexDigit(v[i]);
            if (h<0 || n>=8) { ok=false; break; }
            d[n++]=h;
        }
        const auto byte=[](int hi,int lo){ return static_cast<std::uint8_t>(hi*16+lo); };
        if (ok && n==3) { out=Color{byte(d[0],d[0]),byte(d[1],d[1]),byte(d[2],d[2])}; return PaintColor; }
        if (ok && n==4) { out=Color{byte(d[0],d[0]),byte(d[1],d[1]),byte(d[2],d[2]),byte(d[3],d[3])}; return PaintColor; }
        if (ok && n==6) { out=Color{byte(d[0],d[1]),byte(d[2],d[3]),byte(d[4],d[5])}; return PaintColor; }
        if (ok && n==8) { out=Color{byte(d[0],d[1]),byte(d[2],d[3]),byte(d[4],d[5]),byte(d[6],d[7])}; return PaintColor; }
        return PaintUnknown;
    }
    if (v.compare(0,4,"rgb(")==0 || v.compare(0,5,"rgba(")==0) {
        const std::vector<float> n = numbers(v.substr(v.find('(')+1));
        if (n.size() < 3) return PaintUnknown;
        const bool percent = v.find('%') != std::string::npos;
        const auto ch=[&](float x){ return static_cast<std::uint8_t>(std::lround(std::min(255.f, std::max(0.f, percent ? x*2.55f : x)))); };
        const float alpha = n.size()>3 ? std::min(1.f, std::max(0.f, n[3])) : 1.f;
        out = Color{ch(n[0]), ch(n[1]), ch(n[2]), static_cast<std::uint8_t>(std::lround(alpha*255))};
        return PaintColor;
    }
    for (const Named& c : kNamed)
        if (v == c.name) { out = Color{static_cast<std::uint8_t>(c.rgb>>16), static_cast<std::uint8_t>((c.rgb>>8)&255), static_cast<std::uint8_t>(c.rgb&255)}; return PaintColor; }
    return PaintUnknown;
}
Color faded(Color c, float alpha) {
    c.a = static_cast<std::uint8_t>(std::lround(std::min(1.f, std::max(0.f, alpha)) * c.a));
    return c;
}
Color mixColor(Color a, Color b, float t) {
    const auto m=[&](std::uint8_t x, std::uint8_t y){ return static_cast<std::uint8_t>(std::lround(x + (y-x)*t)); };
    return Color{m(a.r,b.r), m(a.g,b.g), m(a.b,b.b), m(a.a,b.a)};
}

// --- transform lists --------------------------------------------------------
Mat parseTransform(const std::string& s) {
    Mat m = kUnit;
    const char* p = s.c_str(); const char* end = p + s.size();
    while (p < end) {
        while (p<end && (space(*p) || *p==',' || *p==';')) ++p;
        const char* nameStart = p;
        while (p<end && std::isalpha(static_cast<unsigned char>(*p))) ++p;
        if (p == nameStart) { ++p; continue; }
        const std::string name(nameStart, p);
        while (p<end && space(*p)) ++p;
        if (p>=end || *p != '(') continue;
        ++p;
        float v[6]{}; int n=0;
        while (n < 6) { const char* was=p; float x; if (scanNumber(p,end,x)) v[n++]=x; else if (p==was) break; }
        while (p<end && *p != ')') ++p;
        if (p<end) ++p;
        Mat t = kUnit;
        if (name=="translate") t = shift(n>0?v[0]:0.f, n>1?v[1]:0.f);
        else if (name=="scale") { const float sx = n>0?v[0]:1.f; t = Mat{sx,0,0,n>1?v[1]:sx,0,0}; }
        else if (name=="rotate") {
            const float r=(n>0?v[0]:0.f)*kPi/180.f, c=std::cos(r), sn=std::sin(r);
            t = Mat{c,sn,-sn,c,0,0};
            if (n>=3) t = concat(concat(shift(v[1],v[2]), t), shift(-v[1],-v[2]));
        } else if (name=="skewX") t = Mat{1,0,std::tan((n>0?v[0]:0.f)*kPi/180.f),1,0,0};
        else if (name=="skewY") t = Mat{1,std::tan((n>0?v[0]:0.f)*kPi/180.f),0,1,0,0};
        else if (name=="matrix" && n>=6) t = Mat{v[0],v[1],v[2],v[3],v[4],v[5]};
        else continue;
        m = concat(m, t);
    }
    return m;
}

// --- XML --------------------------------------------------------------------
void utf8(std::string& out, unsigned c) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0|(c>>6)); out += static_cast<char>(0x80|(c&0x3F)); }
    else if (c < 0x10000) { out += static_cast<char>(0xE0|(c>>12)); out += static_cast<char>(0x80|((c>>6)&0x3F)); out += static_cast<char>(0x80|(c&0x3F)); }
    else { out += static_cast<char>(0xF0|(c>>18)); out += static_cast<char>(0x80|((c>>12)&0x3F)); out += static_cast<char>(0x80|((c>>6)&0x3F)); out += static_cast<char>(0x80|(c&0x3F)); }
}
std::string decode(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out; out.reserve(s.size());
    for (size_t i=0; i<s.size();) {
        if (s[i] != '&') { out += s[i++]; continue; }
        const size_t semi = s.find(';', i+1);
        const std::string name = (semi != std::string::npos && semi-i <= 10) ? s.substr(i+1, semi-i-1) : std::string();
        if (name=="amp") out += '&';
        else if (name=="lt") out += '<';
        else if (name=="gt") out += '>';
        else if (name=="quot") out += '"';
        else if (name=="apos") out += '\'';
        else if (name.size()>1 && name[0]=='#') {
            const bool hex = name[1]=='x' || name[1]=='X';
            utf8(out, static_cast<unsigned>(std::strtoul(name.c_str()+(hex?2:1), nullptr, hex?16:10)));
        } else { out += s[i++]; continue; }
        i = semi+1;
    }
    return out;
}
bool nameChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c=='_' || c=='-' || c==':' || c=='.' || u>=128;
}
std::string stripNamespace(std::string name) {
    const size_t colon = name.rfind(':');
    return colon == std::string::npos ? name : name.substr(colon+1);
}

struct XNode {
    std::string name, text;
    std::vector<std::pair<std::string,std::string>> attrs;
    std::vector<XNode> kids;
    const std::string* find(const char* key) const {
        for (const auto& a : attrs) if (a.first == key) return &a.second;
        return nullptr;
    }
    float number(const char* key, float fallback = 0) const {
        const std::string* v = find(key);
        return v ? scalar(*v, fallback) : fallback;
    }
};

void attach(std::vector<XNode>& open, std::vector<XNode>& tops, XNode&& node) {
    if (open.empty()) tops.push_back(std::move(node)); else open.back().kids.push_back(std::move(node));
}

// Tolerant enough for real files: declarations, comments, CDATA, doctypes,
// namespaced names, unquoted values, stray or missing close tags.
bool parseXml(const std::string& src, XNode& root, std::vector<std::string>& warnings) {
    std::vector<XNode> open, tops;
    const size_t n = src.size();
    size_t i = 0;
    while (i < n) {
        if (src[i] != '<') {
            const size_t start = i;
            while (i<n && src[i] != '<') ++i;
            if (!open.empty()) open.back().text += decode(src.substr(start, i-start));
            continue;
        }
        if (src.compare(i,4,"<!--")==0) { const size_t e=src.find("-->", i+4); i = (e==std::string::npos)?n:e+3; continue; }
        if (src.compare(i,9,"<![CDATA[")==0) {
            const size_t e = src.find("]]>", i+9);
            if (!open.empty()) open.back().text += src.substr(i+9, (e==std::string::npos?n:e) - (i+9));
            i = (e==std::string::npos)?n:e+3; continue;
        }
        if (src.compare(i,2,"<?")==0) { const size_t e=src.find("?>", i+2); i = (e==std::string::npos)?n:e+2; continue; }
        if (src.compare(i,2,"<!")==0) {
            size_t j=i+2; int depth=0;
            while (j<n) { if (src[j]=='[') ++depth; else if (src[j]==']') --depth; else if (src[j]=='>' && depth<=0) break; ++j; }
            i = (j<n)?j+1:n; continue;
        }
        if (src.compare(i,2,"</")==0) {
            size_t j=i+2;
            while (j<n && space(src[j])) ++j;
            const size_t s=j;
            while (j<n && nameChar(src[j])) ++j;
            const std::string name = stripNamespace(src.substr(s, j-s));
            while (j<n && src[j] != '>') ++j;
            i = (j<n)?j+1:n;
            size_t at = open.size();
            while (at>0 && open[at-1].name != name) --at;
            if (at == 0) { warnings.push_back("SVG: unmatched </" + name + ">"); continue; }
            while (open.size() >= at) { XNode node = std::move(open.back()); open.pop_back(); attach(open, tops, std::move(node)); }
            continue;
        }
        size_t j = i+1;
        const size_t s = j;
        while (j<n && nameChar(src[j])) ++j;
        if (j == s) { ++i; continue; }
        XNode node;
        node.name = stripNamespace(src.substr(s, j-s));
        bool selfClose = false;
        while (j < n) {
            while (j<n && space(src[j])) ++j;
            if (j>=n) break;
            if (src[j]=='/') { selfClose=true; ++j; continue; }
            if (src[j]=='>') { ++j; break; }
            const size_t as=j;
            while (j<n && nameChar(src[j])) ++j;
            if (j == as) { ++j; continue; }
            std::string key = stripNamespace(src.substr(as, j-as));
            std::string value;
            const size_t afterName = j;
            while (j<n && space(src[j])) ++j;
            if (j<n && src[j]=='=') {
                ++j;
                while (j<n && space(src[j])) ++j;
                if (j<n && (src[j]=='"' || src[j]=='\'')) {
                    const char quote = src[j]; const size_t vs = ++j;
                    while (j<n && src[j] != quote) ++j;
                    value = decode(src.substr(vs, j-vs));
                    if (j<n) ++j;
                } else {
                    const size_t vs=j;
                    while (j<n && !space(src[j]) && src[j] != '>' && src[j] != '/') ++j;
                    value = decode(src.substr(vs, j-vs));
                }
            } else j = afterName;
            node.attrs.emplace_back(std::move(key), std::move(value));
        }
        i = j;
        if (selfClose) { attach(open, tops, std::move(node)); continue; }
        if (open.size() >= 128) { warnings.push_back("SVG: nesting too deep, truncated"); break; }
        open.push_back(std::move(node));
    }
    if (!open.empty()) warnings.push_back("SVG: unterminated element <" + open.front().name + ">");
    while (!open.empty()) { XNode node = std::move(open.back()); open.pop_back(); attach(open, tops, std::move(node)); }
    for (XNode& top : tops) if (top.name == "svg") { root = std::move(top); return true; }
    if (tops.empty()) return false;
    root.name = "svg";
    root.kids = std::move(tops);
    return true;
}

// --- CSS --------------------------------------------------------------------
struct CssRule { unsigned char kind = 0; std::string name, decls; };   // 0 tag 1 class 2 id 3 universal

template <class Fn>
void forEachDecl(const std::string& decls, Fn fn) {
    size_t i = 0;
    while (i < decls.size()) {
        const size_t semi = decls.find(';', i);
        const std::string item = decls.substr(i, (semi==std::string::npos ? decls.size() : semi) - i);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) fn(lower(trim(item.substr(0, colon))), trim(item.substr(colon+1)));
        if (semi == std::string::npos) break;
        i = semi+1;
    }
}
void parseCss(const std::string& text, std::vector<CssRule>& out) {
    std::string body; body.reserve(text.size());
    for (size_t i=0; i<text.size();) {
        if (text.compare(i,2,"/*")==0) { const size_t e=text.find("*/", i+2); i = (e==std::string::npos)?text.size():e+2; continue; }
        body += text[i++];
    }
    size_t i = 0;
    while (i < body.size()) {
        const size_t brace = body.find('{', i);
        if (brace == std::string::npos) break;
        const size_t close = body.find('}', brace+1);
        const std::string selectors = body.substr(i, brace-i);
        const std::string decls = body.substr(brace+1, (close==std::string::npos?body.size():close) - brace - 1);
        std::vector<std::string> parts;
        split(selectors, ',', parts);
        for (std::string& sel : parts) {
            if (sel.empty()) continue;
            CssRule rule; rule.decls = decls;
            if (sel[0]=='.') { rule.kind=1; rule.name=sel.substr(1); }
            else if (sel[0]=='#') { rule.kind=2; rule.name=sel.substr(1); }
            else if (sel=="*") rule.kind=3;
            else { rule.kind=0; rule.name=sel; }
            const size_t cut = rule.name.find_first_of(" \t\n\r.#[:>+~");
            if (cut != std::string::npos) rule.name = rule.name.substr(0, cut);
            if (rule.kind==3 || !rule.name.empty()) out.push_back(std::move(rule));
        }
        if (close == std::string::npos) break;
        i = close+1;
    }
}

// --- embedded raster images -------------------------------------------------
// An <image> in these documents carries a whole PNG in a data: URI, so the
// element simply cannot be drawn without decoding one.
//
// The decoder is self-contained on purpose. cpp_canvas links nothing but SDL,
// and taking a zlib dependency so that two mob sprites can draw would push a
// build change onto every consumer of the library. What is here is the subset
// PNG actually needs: RFC 1951 DEFLATE, and 8- or 16-bit non-interlaced
// images. Anything outside that is reported, never guessed at -- a raster
// decoded wrong is worse than a raster not decoded, because it still paints.

struct Raster {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;    // tightly packed, straight (unpremultiplied)
};

int base64Value(char c) {
    if (c>='A' && c<='Z') return c-'A';
    if (c>='a' && c<='z') return c-'a'+26;
    if (c>='0' && c<='9') return c-'0'+52;
    if (c=='+' || c=='-') return 62;   // '-' and '_': the URL-safe alphabet
    if (c=='/' || c=='_') return 63;
    return -1;
}

bool base64Decode(const std::string& text, size_t from, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve((text.size()-from)*3/4 + 3);
    std::uint32_t group = 0;
    int filled = 0;
    for (size_t i=from; i<text.size(); ++i) {
        const char c = text[i];
        if (space(c)) continue;
        if (c=='=') break;
        const int v = base64Value(c);
        if (v < 0) return false;
        group = (group<<6) | static_cast<std::uint32_t>(v);
        if (++filled == 4) { out.push_back(static_cast<std::uint8_t>(group>>16)); out.push_back(static_cast<std::uint8_t>(group>>8)); out.push_back(static_cast<std::uint8_t>(group)); group=0; filled=0; }
    }
    // A trailing group of one is impossible in base64; two and three carry one
    // and two bytes, the padding '=' only says how many.
    if (filled == 1) return false;
    if (filled == 2) out.push_back(static_cast<std::uint8_t>(group>>4));
    else if (filled == 3) { out.push_back(static_cast<std::uint8_t>(group>>10)); out.push_back(static_cast<std::uint8_t>(group>>2)); }
    return !out.empty();
}

// --- DEFLATE (RFC 1951) -----------------------------------------------------
// Canonical-Huffman decoding as in Mark Adler's `puff`: walk the code lengths
// one bit at a time rather than building a lookup table. Slower per symbol,
// but this runs once per document at load, and the table build is the part
// that gets subtly wrong on hand-written inflaters.
struct BitStream {
    const std::uint8_t* data = nullptr;
    size_t size = 0, pos = 0;
    std::uint32_t buffer = 0;
    int filled = 0;
    bool bad = false;
    int take(int need) {
        while (filled < need) {
            if (pos >= size) { bad = true; return 0; }
            buffer |= static_cast<std::uint32_t>(data[pos++]) << filled;
            filled += 8;
        }
        const int value = static_cast<int>(buffer & ((1u<<need)-1u));
        buffer >>= need; filled -= need;
        return value;
    }
};

struct Huffman {
    std::array<int,16> counts{};
    std::vector<std::uint16_t> symbols;
};

bool huffBuild(Huffman& h, const std::uint16_t* lengths, int n) {
    h.counts.fill(0);
    for (int i=0;i<n;++i) ++h.counts[lengths[i] & 15];
    h.counts[0] = 0;
    // Over-subscribed is corrupt; incomplete is legal (a block whose distance
    // table holds a single code, say) and simply never decodes that symbol.
    int left = 1;
    for (int len=1; len<16; ++len) { left <<= 1; left -= h.counts[len]; if (left < 0) return false; }
    std::array<int,16> offset{};
    for (int len=1; len<15; ++len) offset[len+1] = offset[len] + h.counts[len];
    h.symbols.assign(static_cast<size_t>(n), 0);
    for (int i=0;i<n;++i) if (lengths[i]) h.symbols[static_cast<size_t>(offset[lengths[i] & 15]++)] = static_cast<std::uint16_t>(i);
    return true;
}

int huffDecode(BitStream& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len=1; len<16; ++len) {
        code |= s.take(1);
        if (s.bad) return -1;
        const int count = h.counts[len];
        if (code - first < count) return h.symbols[static_cast<size_t>(index + code - first)];
        index += count; first = (first + count) << 1; code <<= 1;
    }
    return -1;
}

const std::uint16_t kLengthBase[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const std::uint16_t kLengthExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const std::uint16_t kDistBase[30]    = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const std::uint16_t kDistExtra[30]   = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool inflateCodes(BitStream& s, const Huffman& lit, const Huffman& dist, std::vector<std::uint8_t>& out, size_t limit) {
    for (;;) {
        const int symbol = huffDecode(s, lit);
        if (symbol < 0) return false;
        if (symbol == 256) return true;
        if (symbol < 256) out.push_back(static_cast<std::uint8_t>(symbol));
        else {
            const int index = symbol - 257;
            if (index >= 29) return false;
            const int length = kLengthBase[index] + s.take(kLengthExtra[index]);
            const int distSymbol = huffDecode(s, dist);
            if (distSymbol < 0 || distSymbol >= 30) return false;
            const size_t distance = static_cast<size_t>(kDistBase[distSymbol]) + static_cast<size_t>(s.take(kDistExtra[distSymbol]));
            if (s.bad || distance == 0 || distance > out.size()) return false;
            const size_t from = out.size() - distance;
            // Byte at a time: an overlapping copy is how run-length encoding
            // falls out of DEFLATE, so memcpy would read what it is writing.
            for (int k=0; k<length; ++k) out.push_back(out[from + static_cast<size_t>(k)]);
        }
        if (out.size() > limit || s.bad) return false;
    }
}

bool inflateRaw(const std::uint8_t* data, size_t size, size_t limit, std::vector<std::uint8_t>& out) {
    BitStream s{data, size, 0, 0, 0, false};
    static const std::array<Huffman,2> fixed = []{
        std::array<Huffman,2> tables;
        std::uint16_t lengths[288];
        for (int i=0;i<144;++i) lengths[i]=8;
        for (int i=144;i<256;++i) lengths[i]=9;
        for (int i=256;i<280;++i) lengths[i]=7;
        for (int i=280;i<288;++i) lengths[i]=8;
        huffBuild(tables[0], lengths, 288);
        for (int i=0;i<30;++i) lengths[i]=5;
        huffBuild(tables[1], lengths, 30);
        return tables;
    }();
    for (;;) {
        const int last = s.take(1);
        const int type = s.take(2);
        if (s.bad) return false;
        if (type == 0) {
            s.take(s.filled & 7);                      // stored data starts on a byte boundary
            const int lo = s.take(8), hi = s.take(8);
            s.take(8); s.take(8);                      // NLEN, the one's complement of LEN
            const int length = lo | (hi<<8);
            if (s.bad || out.size() + static_cast<size_t>(length) > limit) return false;
            for (int i=0;i<length;++i) out.push_back(static_cast<std::uint8_t>(s.take(8)));
            if (s.bad) return false;
        } else if (type == 1) {
            if (!inflateCodes(s, fixed[0], fixed[1], out, limit)) return false;
        } else if (type == 2) {
            const int litCount = s.take(5) + 257, distCount = s.take(5) + 1, codeCount = s.take(4) + 4;
            if (s.bad || litCount > 286 || distCount > 30) return false;
            static const int kOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::uint16_t codeLengths[19] = {0};
            for (int i=0;i<codeCount;++i) codeLengths[kOrder[i]] = static_cast<std::uint16_t>(s.take(3));
            Huffman code;
            if (s.bad || !huffBuild(code, codeLengths, 19)) return false;
            std::uint16_t lengths[320] = {0};
            const int total = litCount + distCount;
            int at = 0;
            while (at < total) {
                const int symbol = huffDecode(s, code);
                if (symbol < 0) return false;
                if (symbol < 16) lengths[at++] = static_cast<std::uint16_t>(symbol);
                else {
                    std::uint16_t value = 0;
                    int repeat;
                    if (symbol == 16) { if (at == 0) return false; value = lengths[at-1]; repeat = 3 + s.take(2); }
                    else if (symbol == 17) repeat = 3 + s.take(3);
                    else repeat = 11 + s.take(7);
                    if (s.bad || at + repeat > total) return false;
                    while (repeat-- > 0) lengths[at++] = value;
                }
            }
            Huffman lit, dist;
            if (!huffBuild(lit, lengths, litCount) || !huffBuild(dist, lengths + litCount, distCount)) return false;
            if (!inflateCodes(s, lit, dist, out, limit)) return false;
        } else return false;
        if (last) return true;
    }
}

// --- PNG --------------------------------------------------------------------
std::uint32_t beU32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0])<<24) | (static_cast<std::uint32_t>(p[1])<<16) |
           (static_cast<std::uint32_t>(p[2])<<8) | p[3];
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p-a), pb = std::abs(p-b), pc = std::abs(p-c);
    return (pa<=pb && pa<=pc) ? a : (pb<=pc ? b : c);
}

bool decodePng(const std::vector<std::uint8_t>& file, Raster& out, std::string& error) {
    static const std::uint8_t kSignature[8] = {137,80,78,71,13,10,26,10};
    if (file.size() < 8 || !std::equal(kSignature, kSignature+8, file.begin())) { error = "not a PNG"; return false; }

    std::uint32_t width=0, height=0;
    int depth=0, colorType=0, interlace=0;
    std::vector<std::uint8_t> deflated, palette, alphaTable;
    bool sawHeader = false;

    for (size_t at = 8; at + 8 <= file.size();) {
        const std::uint32_t length = beU32(&file[at]);
        if (length > file.size() || at + 12 + length > file.size()) { error = "truncated chunk"; return false; }
        const std::uint8_t* body = &file[at+8];
        const char* tag = reinterpret_cast<const char*>(&file[at+4]);
        if (std::equal(tag, tag+4, "IHDR")) {
            if (length < 13) { error = "short IHDR"; return false; }
            width = beU32(body); height = beU32(body+4);
            depth = body[8]; colorType = body[9]; interlace = body[12];
            if (body[10] != 0 || body[11] != 0) { error = "unknown compression or filter method"; return false; }
            sawHeader = true;
        } else if (std::equal(tag, tag+4, "PLTE")) palette.assign(body, body+length);
        else if (std::equal(tag, tag+4, "tRNS")) alphaTable.assign(body, body+length);
        else if (std::equal(tag, tag+4, "IDAT")) deflated.insert(deflated.end(), body, body+length);
        else if (std::equal(tag, tag+4, "IEND")) break;
        at += 12 + length;
    }
    if (!sawHeader || width == 0 || height == 0) { error = "no image header"; return false; }
    if (width > 8192 || height > 8192) { error = "image too large"; return false; }
    if (interlace != 0) { error = "interlaced PNGs are not supported"; return false; }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) { error = "unknown bit depth"; return false; }
    int channels = 0;
    if (colorType == 0) channels = 1;
    else if (colorType == 2) channels = 3;
    else if (colorType == 3) channels = 1;
    else if (colorType == 4) channels = 2;
    else if (colorType == 6) channels = 4;
    else { error = "unknown colour type"; return false; }
    if (depth < 8 && colorType != 0 && colorType != 3) { error = "sub-byte samples need greyscale or a palette"; return false; }
    if (deflated.size() < 3) { error = "no image data"; return false; }
    // zlib wrapper: two header bytes, and a preset dictionary this never uses.
    if ((deflated[0] & 0x0F) != 8 || (deflated[1] & 0x20) != 0) { error = "unsupported zlib stream"; return false; }

    const int sampleBytes = depth/8;
    // The filter's "pixel to the left" is measured in whole bytes, and rounds
    // UP to one for the sub-byte depths -- getting that wrong decodes every
    // filtered 1/2/4-bit row into garbage that still looks like an image.
    const size_t pixelBytes = std::max<size_t>(1, static_cast<size_t>(channels) * sampleBytes);
    const size_t stride = depth >= 8 ? static_cast<size_t>(width) * pixelBytes
                                     : (static_cast<size_t>(width) * depth + 7) / 8;
    const size_t expected = (stride + 1) * height;
    std::vector<std::uint8_t> raw;
    raw.reserve(expected);
    if (!inflateRaw(deflated.data()+2, deflated.size()-2, expected, raw) || raw.size() < expected) {
        error = "compressed data is corrupt";
        return false;
    }

    // Unfilter in place, row by row: every filter refers to the row above it,
    // so this has to run top to bottom before anything can be sampled.
    std::vector<std::uint8_t> lines(stride * height);
    for (std::uint32_t y=0; y<height; ++y) {
        const std::uint8_t filter = raw[(stride+1)*y];
        const std::uint8_t* src = &raw[(stride+1)*y + 1];
        std::uint8_t* line = &lines[stride*y];
        const std::uint8_t* above = y ? &lines[stride*(y-1)] : nullptr;
        for (size_t x=0; x<stride; ++x) {
            const int left = x >= pixelBytes ? line[x-pixelBytes] : 0;
            const int up = above ? above[x] : 0;
            const int upLeft = (above && x >= pixelBytes) ? above[x-pixelBytes] : 0;
            int value = src[x];
            switch (filter) {
            case 0: break;
            case 1: value += left; break;
            case 2: value += up; break;
            case 3: value += (left + up)/2; break;
            case 4: value += paeth(left, up, upLeft); break;
            default: error = "unknown row filter"; return false;
            }
            line[x] = static_cast<std::uint8_t>(value & 0xFF);
        }
    }

    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    for (std::uint32_t y=0; y<height; ++y) {
        const std::uint8_t* line = &lines[stride*y];
        for (std::uint32_t x=0; x<width; ++x) {
            // 16-bit samples keep their high byte: the canvas is 8-bit, and the
            // low byte is below anything it can represent.
            std::uint8_t packed = 0;
            const std::uint8_t* s = line + static_cast<size_t>(x)*pixelBytes;
            if (depth < 8) {
                const size_t bit = static_cast<size_t>(x) * depth;
                packed = static_cast<std::uint8_t>((line[bit/8] >> (8 - depth - static_cast<int>(bit%8))) & ((1<<depth)-1));
                s = &packed;
            }
            std::uint8_t* d = &out.rgba[(static_cast<size_t>(y)*width + x)*4];
            if (colorType == 3) {
                const size_t index = s[0];
                const bool known = index*3 + 2 < palette.size();
                d[0] = known ? palette[index*3] : 0;
                d[1] = known ? palette[index*3+1] : 0;
                d[2] = known ? palette[index*3+2] : 0;
                d[3] = index < alphaTable.size() ? alphaTable[index] : 255;
            } else if (colorType == 0) {
                // Greyscale below 8 bits spans the same 0..255 range, so the
                // sample scales rather than shifts: 1-bit white is 255, not 1.
                const int grey = depth < 8 ? s[0] * 255 / ((1<<depth)-1) : s[0];
                d[0]=d[1]=d[2]=static_cast<std::uint8_t>(grey); d[3]=255;
            }
            else if (colorType == 2) { d[0]=s[0]; d[1]=s[sampleBytes]; d[2]=s[sampleBytes*2]; d[3]=255; }
            else if (colorType == 4) { d[0]=d[1]=d[2]=s[0]; d[3]=s[sampleBytes]; }
            else { d[0]=s[0]; d[1]=s[sampleBytes]; d[2]=s[sampleBytes*2]; d[3]=s[sampleBytes*3]; }
        }
    }
    return true;
}

// --- resolved presentation state -------------------------------------------
struct Style {
    Color fill{0,0,0,255}, stroke{0,0,0,255};
    std::vector<float> dash;
    float fillOpacity=1, strokeOpacity=1, strokeWidth=1, miterLimit=4, dashOffset=0;
    bool fillOn=true, strokeOn=false, fillEvenOdd=false, clipEvenOdd=false;
    unsigned char cap=0, join=0;
};
const char* kCapName[3] = {"butt", "round", "square"};
const char* kJoinName[3] = {"miter", "round", "bevel"};

enum : unsigned char { ShapeNone, ShapeRect, ShapeCircle, ShapeEllipse, ShapeLine, ShapePolyline, ShapePolygon, ShapePath };
enum : unsigned char { TgtNone, TgtGeom, TgtOpacity, TgtFillOpacity, TgtStrokeOpacity, TgtStrokeWidth, TgtFill, TgtStroke, TgtPathData, TgtTransform };
enum : unsigned char { TrTranslate, TrScale, TrRotate, TrSkewX, TrSkewY };

struct Anim {
    std::vector<float> keyTimes, values;
    std::vector<std::array<float,4>> splines;
    std::vector<Color> colors;
    std::vector<Path2D> paths;
    float begin=0, dur=0, repeat=1;
    int stride=1;
    unsigned char target=TgtNone, slot=0, type=TrTranslate;
    bool additive=false, freeze=false, discrete=false, spline=false;
};

struct Node {
    Style style;
    Mat transform = kUnit;
    Path2D path;
    std::vector<float> points;
    std::vector<Anim> anims;
    std::vector<Node> kids;
    // Decoded <image> pixels. Shared, not copied: the same sprite is compiled
    // once and the tree is copied into the retained Scene.
    std::shared_ptr<const Raster> image;
    float imageBox[4]{0,0,0,0};    ///< where the pixels land, after preserveAspectRatio
    float imageClip[4]{0,0,0,0};   ///< the declared box; only 'slice' draws outside it
    float geom[6]{0,0,0,0,-1,-1};
    float opacity = 1;
    int clip = -1;
    unsigned char shape = ShapeNone;
    bool transformed = false, dynamic = false, imageSlice = false;
};

struct Clip { Path2D path; bool evenOdd = false; };

// --- geometry ---------------------------------------------------------------
void buildShape(unsigned char shape, const float* g, const std::vector<float>& pts, Path2D& out) {
    out.clear();
    switch (shape) {
    case ShapeRect: {
        const float x=g[0], y=g[1], w=g[2], h=g[3];
        if (!(w>0) || !(h>0)) return;
        float rx=g[4], ry=g[5];
        if (rx<0 && ry<0) { out.rect(x,y,w,h); return; }
        if (rx<0) rx=ry;
        if (ry<0) ry=rx;
        rx = std::min(std::max(rx,0.f), w/2); ry = std::min(std::max(ry,0.f), h/2);
        if (rx<=0 || ry<=0) { out.rect(x,y,w,h); return; }
        out.moveTo(x+rx, y);
        out.lineTo(x+w-rx, y); out.ellipse(x+w-rx, y+ry,   rx, ry, 0, -kPi/2, 0);
        out.lineTo(x+w, y+h-ry); out.ellipse(x+w-rx, y+h-ry, rx, ry, 0, 0, kPi/2);
        out.lineTo(x+rx, y+h);  out.ellipse(x+rx,   y+h-ry, rx, ry, 0, kPi/2, kPi);
        out.lineTo(x, y+ry);    out.ellipse(x+rx,   y+ry,   rx, ry, 0, kPi, kPi*1.5f);
        out.closePath();
        return; }
    case ShapeCircle:
        if (g[2] > 0) { out.ellipse(g[0], g[1], g[2], g[2], 0, 0, kTau); out.closePath(); }
        return;
    case ShapeEllipse:
        if (g[2] > 0 && g[3] > 0) { out.ellipse(g[0], g[1], g[2], g[3], 0, 0, kTau); out.closePath(); }
        return;
    case ShapeLine:
        out.moveTo(g[0], g[1]); out.lineTo(g[2], g[3]);
        return;
    case ShapePolyline:
    case ShapePolygon:
        if (pts.size() >= 4) {
            out.moveTo(pts[0], pts[1]);
            for (size_t i=2; i+1<pts.size(); i+=2) out.lineTo(pts[i], pts[i+1]);
            if (shape == ShapePolygon) out.closePath();
        }
        return;
    default: return;
    }
}

// F.6.5 endpoint -> centre parameterisation; emitted as a real canvas ellipse
// arc rather than the straight line the old compiler drew.
void arcTo(Path2D& out, float x1, float y1, float rx, float ry, float phiDeg, bool large, bool sweep, float x2, float y2) {
    if (!(rx != 0) || !(ry != 0) || (x1==x2 && y1==y2)) { out.lineTo(x2, y2); return; }
    rx = std::abs(rx); ry = std::abs(ry);
    const float phi = phiDeg*kPi/180.f, cs = std::cos(phi), sn = std::sin(phi);
    const float dx = (x1-x2)/2, dy = (y1-y2)/2;
    const float ax =  cs*dx + sn*dy, ay = -sn*dx + cs*dy;
    const float lambda = ax*ax/(rx*rx) + ay*ay/(ry*ry);
    if (lambda > 1) { const float k = std::sqrt(lambda); rx *= k; ry *= k; }
    const float num = rx*rx*ry*ry - rx*rx*ay*ay - ry*ry*ax*ax;
    const float den = rx*rx*ay*ay + ry*ry*ax*ax;
    float scale = den > 0 ? std::sqrt(std::max(0.f, num/den)) : 0.f;
    if (large == sweep) scale = -scale;
    const float cxp = scale*rx*ay/ry, cyp = -scale*ry*ax/rx;
    const float cx = cs*cxp - sn*cyp + (x1+x2)/2;
    const float cy = sn*cxp + cs*cyp + (y1+y2)/2;
    const float ux = (ax-cxp)/rx, uy = (ay-cyp)/ry, vx = (-ax-cxp)/rx, vy = (-ay-cyp)/ry;
    const float start = std::atan2(uy, ux);
    float sweepAngle = std::atan2(ux*vy - uy*vx, ux*vx + uy*vy);
    if (!sweep && sweepAngle > 0) sweepAngle -= kTau;
    else if (sweep && sweepAngle < 0) sweepAngle += kTau;
    out.ellipse(cx, cy, rx, ry, phi, start, start + sweepAngle, !sweep);
}

int parsePathData(const std::string& d, Path2D& out) {
    out.clear();
    const char* p = d.c_str(); const char* end = p + d.size();
    float x=0, y=0, sx=0, sy=0, cx2=0, cy2=0, qx=0, qy=0;
    char command = 0, previous = 0;
    int bad = 0;
    const auto pair=[&](float& a, float& b, bool relative) {
        if (!scanNumber(p, end, a) || !scanNumber(p, end, b)) return false;
        if (relative) { a += x; b += y; }
        return true;
    };
    for (;;) {
        while (p<end && (space(*p) || *p==',')) ++p;
        if (p >= end) break;
        if (std::isalpha(static_cast<unsigned char>(*p))) command = *p++;
        else if (!command) { ++p; ++bad; continue; }
        const bool rel = std::islower(static_cast<unsigned char>(command)) != 0;
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        bool ok = true;
        switch (upper) {
        case 'M': { float a,b; if (!(ok=pair(a,b,rel))) break; out.moveTo(a,b); x=sx=a; y=sy=b; command = rel?'l':'L'; break; }
        case 'L': { float a,b; if (!(ok=pair(a,b,rel))) break; out.lineTo(a,b); x=a; y=b; break; }
        case 'H': { float a; if (!(ok=scanNumber(p,end,a))) break; if (rel) a+=x; out.lineTo(a,y); x=a; break; }
        case 'V': { float a; if (!(ok=scanNumber(p,end,a))) break; if (rel) a+=y; out.lineTo(x,a); y=a; break; }
        case 'C': { float a,b,c,e,f,g; if (!(ok = pair(a,b,rel) && pair(c,e,rel) && pair(f,g,rel))) break;
                    out.bezierCurveTo(a,b,c,e,f,g); cx2=c; cy2=e; x=f; y=g; break; }
        case 'S': { float c,e,f,g; if (!(ok = pair(c,e,rel) && pair(f,g,rel))) break;
                    const bool wasCubic = previous=='C' || previous=='S';
                    const float a = wasCubic ? 2*x-cx2 : x, b = wasCubic ? 2*y-cy2 : y;
                    out.bezierCurveTo(a,b,c,e,f,g); cx2=c; cy2=e; x=f; y=g; break; }
        case 'Q': { float a,b,c,e; if (!(ok = pair(a,b,rel) && pair(c,e,rel))) break;
                    out.quadraticCurveTo(a,b,c,e); qx=a; qy=b; x=c; y=e; break; }
        case 'T': { float c,e; if (!(ok=pair(c,e,rel))) break;
                    const bool wasQuad = previous=='Q' || previous=='T';
                    const float a = wasQuad ? 2*x-qx : x, b = wasQuad ? 2*y-qy : y;
                    out.quadraticCurveTo(a,b,c,e); qx=a; qy=b; x=c; y=e; break; }
        case 'A': { float rx,ry,rot,ex,ey; bool large,sweep;
                    if (!(ok = scanNumber(p,end,rx) && scanNumber(p,end,ry) && scanNumber(p,end,rot) &&
                               scanFlag(p,end,large) && scanFlag(p,end,sweep) && pair(ex,ey,rel))) break;
                    arcTo(out, x, y, rx, ry, rot, large, sweep, ex, ey); x=ex; y=ey; break; }
        case 'Z': out.closePath(); x=sx; y=sy; break;
        default: ok = false; ++bad; command = 0; break;
        }
        if (!ok) { command = 0; continue; }
        previous = upper;
    }
    return bad;
}

// Baking a path down to lines and curves so a matrix can be folded into the
// coordinates; needed because clipPath children carry their own transforms.
void arcCubics(Path2D& out, float cx, float cy, float rx, float ry, float rot, float start, float sweep) {
    const int n = std::max(1, static_cast<int>(std::ceil(std::abs(sweep)/(kPi/2))));
    const float step = sweep/n, k = 4.f/3.f*std::tan(step/4);
    const float cs = std::cos(rot), sn = std::sin(rot);
    const auto at=[&](float t, float& px, float& py){ const float a=rx*std::cos(t), b=ry*std::sin(t); px=cx+a*cs-b*sn; py=cy+a*sn+b*cs; };
    const auto tangent=[&](float t, float& px, float& py){ const float a=-rx*std::sin(t), b=ry*std::cos(t); px=a*cs-b*sn; py=a*sn+b*cs; };
    float a = start, x0, y0; at(a, x0, y0);
    for (int i=0; i<n; ++i) {
        const float b = a + step;
        float x1, y1, t0x, t0y, t1x, t1y;
        at(b, x1, y1); tangent(a, t0x, t0y); tangent(b, t1x, t1y);
        out.bezierCurveTo(x0 + k*t0x, y0 + k*t0y, x1 - k*t1x, y1 - k*t1y, x1, y1);
        a = b; x0 = x1; y0 = y1;
    }
}
void appendBaked(Path2D& dst, const Path2D& src, const Mat& m) {
    const size_t base = dst.segments().size();
    bool open = false;
    float cx=0, cy=0;
    for (const Path2D::Segment& s : src.segments()) {
        const float* v = s.v;
        switch (s.command) {
        case Path2D::Command::Move: dst.moveTo(v[0], v[1]); open=true; cx=v[0]; cy=v[1]; break;
        case Path2D::Command::Line: if (!open) { dst.moveTo(v[0],v[1]); open=true; } else dst.lineTo(v[0], v[1]); cx=v[0]; cy=v[1]; break;
        case Path2D::Command::Quadratic: if (!open) { dst.moveTo(cx,cy); open=true; } dst.quadraticCurveTo(v[0],v[1],v[2],v[3]); cx=v[2]; cy=v[3]; break;
        case Path2D::Command::Bezier: if (!open) { dst.moveTo(cx,cy); open=true; } dst.bezierCurveTo(v[0],v[1],v[2],v[3],v[4],v[5]); cx=v[4]; cy=v[5]; break;
        case Path2D::Command::Arc:
        case Path2D::Command::Ellipse: {
            const bool circle = s.command == Path2D::Command::Arc;
            const float rx = std::abs(v[2]), ry = circle ? std::abs(v[2]) : std::abs(v[3]);
            const float rot = circle ? 0.f : v[4], a0 = circle ? v[3] : v[5], a1 = circle ? v[4] : v[6];
            float sweep = a1 - a0;
            if (!s.counterClockwise) { if (sweep >= kTau) sweep = kTau; else { sweep = std::fmod(sweep, kTau); if (sweep < 0) sweep += kTau; } }
            else { if (sweep <= -kTau) sweep = -kTau; else { sweep = std::fmod(sweep, kTau); if (sweep > 0) sweep -= kTau; } }
            const float cs = std::cos(rot), sn = std::sin(rot);
            const float px = v[0] + rx*std::cos(a0)*cs - ry*std::sin(a0)*sn;
            const float py = v[1] + rx*std::cos(a0)*sn + ry*std::sin(a0)*cs;
            if (!open) { dst.moveTo(px, py); open = true; } else dst.lineTo(px, py);
            arcCubics(dst, v[0], v[1], rx, ry, rot, a0, sweep);
            const float qx = v[0] + rx*std::cos(a0+sweep)*cs - ry*std::sin(a0+sweep)*sn;
            const float qy = v[1] + rx*std::cos(a0+sweep)*sn + ry*std::sin(a0+sweep)*cs;
            cx = qx; cy = qy; break; }
        case Path2D::Command::ArcTo: if (!open) { dst.moveTo(v[0],v[1]); open=true; } else dst.lineTo(v[0], v[1]); dst.lineTo(v[2], v[3]); cx=v[2]; cy=v[3]; break;
        case Path2D::Command::Rect:
            dst.moveTo(v[0],v[1]); dst.lineTo(v[0]+v[2],v[1]); dst.lineTo(v[0]+v[2],v[1]+v[3]); dst.lineTo(v[0],v[1]+v[3]); dst.closePath();
            open = false; cx=v[0]; cy=v[1]; break;
        case Path2D::Command::RoundRect: {
            const float x=std::min(v[0],v[0]+v[2]), y=std::min(v[1],v[1]+v[3]);
            const float w=std::abs(v[2]), h=std::abs(v[3]), r=std::min(std::max(v[4],0.f), std::min(w,h)/2);
            dst.moveTo(x+r, y);
            dst.lineTo(x+w-r, y); arcCubics(dst, x+w-r, y+r, r, r, 0, -kPi/2, kPi/2);
            dst.lineTo(x+w, y+h-r); arcCubics(dst, x+w-r, y+h-r, r, r, 0, 0, kPi/2);
            dst.lineTo(x+r, y+h); arcCubics(dst, x+r, y+h-r, r, r, 0, kPi/2, kPi/2);
            dst.lineTo(x, y+r); arcCubics(dst, x+r, y+r, r, r, 0, kPi, kPi/2);
            dst.closePath(); open=false; cx=x; cy=y; break; }
        case Path2D::Command::Close: dst.closePath(); open=false; cx=0; cy=0; break;
        }
    }
    std::vector<Path2D::Segment>& segs = dst.segments();
    for (size_t i=base; i<segs.size(); ++i) {
        int count = 0;
        switch (segs[i].command) {
        case Path2D::Command::Move: case Path2D::Command::Line: count=1; break;
        case Path2D::Command::Quadratic: count=2; break;
        case Path2D::Command::Bezier: count=3; break;
        default: count=0; break;
        }
        for (int k=0; k<count; ++k) {
            const float px = segs[i].v[k*2], py = segs[i].v[k*2+1];
            segs[i].v[k*2]   = m[0]*px + m[2]*py + m[4];
            segs[i].v[k*2+1] = m[1]*px + m[3]*py + m[5];
        }
    }
}

// --- SMIL -------------------------------------------------------------------
float easeSpline(const std::array<float,4>& s, float u) {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    float lo=0, hi=1, t=u;
    for (int i=0; i<24; ++i) {
        const float mt = 1-t;
        const float x = 3*mt*mt*t*s[0] + 3*mt*t*t*s[2] + t*t*t;
        if (x < u) lo = t; else hi = t;
        t = (lo+hi)/2;
    }
    const float mt = 1-t;
    return 3*mt*mt*t*s[1] + 3*mt*t*t*s[3] + t*t*t;
}
bool animPhase(const Anim& a, float time, int& key, float& u) {
    const int n = static_cast<int>(a.keyTimes.size());
    if (n == 0 || a.dur <= 0) return false;
    const float t = time - a.begin;
    if (t < 0) return false;
    float f;
    if (a.repeat >= 0 && t >= a.dur*a.repeat) { if (!a.freeze) return false; f = 1; }
    else f = std::fmod(t, a.dur) / a.dur;
    if (n == 1) { key = 0; u = 0; return true; }
    key = 0;
    while (key+2 < n && f >= a.keyTimes[key+1]) ++key;
    const float t0 = a.keyTimes[key], t1 = a.keyTimes[key+1];
    u = t1 > t0 ? (f-t0)/(t1-t0) : 0.f;
    u = std::min(1.f, std::max(0.f, u));
    if (a.discrete) u = 0;
    else if (a.spline && key < static_cast<int>(a.splines.size())) u = easeSpline(a.splines[key], u);
    return true;
}
void animValues(const Anim& a, int key, float u, float* out) {
    const int keys = static_cast<int>(a.values.size()) / std::max(1, a.stride);
    const int i = std::min(key, std::max(0, keys-1));
    const int j = std::min(key+1, std::max(0, keys-1));
    for (int k=0; k<a.stride; ++k) {
        const float v0 = a.values[i*a.stride + k], v1 = a.values[j*a.stride + k];
        out[k] = v0 + (v1-v0)*u;
    }
}
Mat transformOf(unsigned char type, const float* v) {
    switch (type) {
    case TrScale: return Mat{v[0], 0, 0, v[1], 0, 0};
    case TrRotate: {
        const float r = v[0]*kPi/180.f, c = std::cos(r), s = std::sin(r);
        Mat m{c, s, -s, c, 0, 0};
        if (v[1] != 0 || v[2] != 0) m = concat(concat(shift(v[1], v[2]), m), shift(-v[1], -v[2]));
        return m; }
    case TrSkewX: return Mat{1, 0, std::tan(v[0]*kPi/180.f), 1, 0, 0};
    case TrSkewY: return Mat{1, std::tan(v[0]*kPi/180.f), 0, 1, 0, 0};
    default: return shift(v[0], v[1]);
    }
}

// --- scene construction -----------------------------------------------------
struct Props {
    Style style;
    std::string transform, origin, clipRef;
    float opacity = 1;
    bool visible = true;
};

bool geomSlot(unsigned char shape, const std::string& name, unsigned char& slot) {
    static const char* rect[6]    = {"x","y","width","height","rx","ry"};
    static const char* circle[3]  = {"cx","cy","r"};
    static const char* ellipse[4] = {"cx","cy","rx","ry"};
    static const char* line[4]    = {"x1","y1","x2","y2"};
    const char** table = nullptr; int n = 0;
    if (shape==ShapeRect) { table=rect; n=6; }
    else if (shape==ShapeCircle) { table=circle; n=3; }
    else if (shape==ShapeEllipse) { table=ellipse; n=4; }
    else if (shape==ShapeLine) { table=line; n=4; }
    for (int i=0; i<n; ++i) if (name == table[i]) { slot = static_cast<unsigned char>(i); return true; }
    return false;
}

struct Builder {
    std::vector<Clip>& clips;
    std::vector<std::string>& warnings;
    std::unordered_map<std::string, const XNode*> ids;
    std::unordered_map<std::string, int> clipCache;
    std::vector<CssRule> css;

    void warn(const std::string& message) {
        for (const std::string& w : warnings) if (w == message) return;
        if (warnings.size() < 48) warnings.push_back(message);
    }
    void collect(const XNode& x, int depth) {
        if (depth > 64) return;
        if (const std::string* id = x.find("id")) if (!id->empty()) ids.emplace(*id, &x);
        if (x.name == "style") parseCss(x.text, css);
        for (const XNode& kid : x.kids) collect(kid, depth+1);
    }

    void applyDecl(Props& p, const std::string& key, const std::string& value) {
        Style& s = p.style;
        if (key=="fill") {
            Color c;
            const unsigned char kind = parsePaint(value, c);
            if (kind==PaintNone) s.fillOn = false;
            else if (kind==PaintColor) { s.fill = c; s.fillOn = true; }
            else warn("SVG: unsupported fill paint: " + value);
        } else if (key=="stroke") {
            Color c;
            const unsigned char kind = parsePaint(value, c);
            if (kind==PaintNone) s.strokeOn = false;
            else if (kind==PaintColor) { s.stroke = c; s.strokeOn = true; }
            else warn("SVG: unsupported stroke paint: " + value);
        }
        else if (key=="fill-opacity") s.fillOpacity = std::min(1.f, std::max(0.f, scalar(value, 1)));
        else if (key=="stroke-opacity") s.strokeOpacity = std::min(1.f, std::max(0.f, scalar(value, 1)));
        else if (key=="opacity") p.opacity = std::min(1.f, std::max(0.f, scalar(value, 1)));
        else if (key=="stroke-width") s.strokeWidth = std::max(0.f, scalar(value, 1));
        else if (key=="stroke-miterlimit") s.miterLimit = std::max(1.f, scalar(value, 4));
        else if (key=="stroke-dashoffset") s.dashOffset = scalar(value, 0);
        else if (key=="stroke-linecap") s.cap = value=="round" ? 1 : value=="square" ? 2 : 0;
        else if (key=="stroke-linejoin") s.join = value=="round" ? 1 : value=="bevel" ? 2 : 0;
        else if (key=="stroke-dasharray") {
            s.dash.clear();
            if (value!="none" && !value.empty()) {
                numbersInto(value, s.dash);
                bool positive = false;
                for (float d : s.dash) if (d > 0) positive = true;
                if (!positive) s.dash.clear();
            }
        }
        else if (key=="fill-rule") s.fillEvenOdd = value=="evenodd";
        else if (key=="clip-rule") s.clipEvenOdd = value=="evenodd";
        else if (key=="clip-path") p.clipRef = trim(value);
        else if (key=="transform") p.transform = value;
        else if (key=="transform-origin") p.origin = value;
        else if (key=="display") { if (value=="none") p.visible = false; }
        else if (key=="visibility") { if (value=="hidden" || value=="collapse") p.visible = false; }
    }

    void resolve(const XNode& x, const Style& parent, Props& p) {
        p.style = parent;
        for (const auto& a : x.attrs) applyDecl(p, a.first, a.second);
        if (!css.empty()) {
            const std::string* cls = x.find("class");
            const std::string* id = x.find("id");
            const unsigned char order[4] = {3, 0, 1, 2};
            for (unsigned char kind : order)
                for (const CssRule& rule : css) {
                    if (rule.kind != kind) continue;
                    const bool match = kind==3 || (kind==0 && rule.name==x.name) ||
                                       (kind==1 && cls && hasWord(*cls, rule.name)) || (kind==2 && id && *id==rule.name);
                    if (match) forEachDecl(rule.decls, [&](const std::string& a, const std::string& b){ applyDecl(p, a, b); });
                }
        }
        if (const std::string* inline_ = x.find("style"))
            forEachDecl(*inline_, [&](const std::string& a, const std::string& b){ applyDecl(p, a, b); });
    }

    void bakeClip(const XNode& x, const Mat& parent, Path2D& out, int depth) {
        if (depth > 16) return;
        for (const XNode& kid : x.kids) {
            const std::string* t = kid.find("transform");
            const Mat m = t ? concat(parent, parseTransform(*t)) : parent;
            if (kid.name=="use") {
                const std::string* href = kid.find("href");
                if (href && href->size()>1 && (*href)[0]=='#') {
                    auto it = ids.find(href->substr(1));
                    if (it != ids.end()) {
                        XNode wrapper; wrapper.kids.push_back(*it->second);
                        bakeClip(wrapper, concat(m, shift(kid.number("x"), kid.number("y"))), out, depth+1);
                    }
                }
                continue;
            }
            if (kid.name=="g") { bakeClip(kid, m, out, depth+1); continue; }
            Path2D local;
            if (kid.name=="path") { if (const std::string* d = kid.find("d")) parsePathData(*d, local); }
            else {
                float g[6] = {0,0,0,0,-1,-1};
                std::vector<float> pts;
                unsigned char shape = ShapeNone;
                if (kid.name=="rect") { shape=ShapeRect; g[0]=kid.number("x"); g[1]=kid.number("y"); g[2]=kid.number("width"); g[3]=kid.number("height"); g[4]=kid.find("rx")?kid.number("rx"):-1.f; g[5]=kid.find("ry")?kid.number("ry"):-1.f; }
                else if (kid.name=="circle") { shape=ShapeCircle; g[0]=kid.number("cx"); g[1]=kid.number("cy"); g[2]=kid.number("r"); }
                else if (kid.name=="ellipse") { shape=ShapeEllipse; g[0]=kid.number("cx"); g[1]=kid.number("cy"); g[2]=kid.number("rx"); g[3]=kid.number("ry"); }
                else if (kid.name=="polygon" || kid.name=="polyline") { shape = kid.name=="polygon" ? ShapePolygon : ShapePolyline; if (const std::string* v = kid.find("points")) numbersInto(*v, pts); }
                if (shape == ShapeNone) continue;
                buildShape(shape, g, pts, local);
            }
            if (local.empty()) continue;
            appendBaked(out, local, m);
        }
    }

    int clipFor(const std::string& reference) {
        const size_t hash = reference.find('#');
        if (reference.compare(0,4,"url(") != 0 || hash == std::string::npos) {
            if (reference != "none" && !reference.empty()) warn("SVG: unsupported clip-path: " + reference);
            return -1;
        }
        size_t stop = reference.find_first_of(")'\"", hash+1);
        const std::string id = trim(reference.substr(hash+1, (stop==std::string::npos ? reference.size() : stop) - hash - 1));
        auto cached = clipCache.find(id);
        if (cached != clipCache.end()) return cached->second;
        auto it = ids.find(id);
        if (it == ids.end() || it->second->name != "clipPath") { warn("SVG: clipPath not found: #" + id); return -1; }
        const XNode& node = *it->second;
        if (const std::string* units = node.find("clipPathUnits"))
            if (*units == "objectBoundingBox") warn("SVG: clipPathUnits=objectBoundingBox is not supported");
        Clip clip;
        clip.evenOdd = node.find("clip-rule") && *node.find("clip-rule") == "evenodd";
        const std::string* t = node.find("transform");
        bakeClip(node, t ? parseTransform(*t) : kUnit, clip.path, 0);
        int index = -1;
        if (!clip.path.empty()) { index = static_cast<int>(clips.size()); clips.push_back(std::move(clip)); }
        clipCache.emplace(id, index);
        return index;
    }

    bool parseAnim(const XNode& x, Node& owner, Anim& a) {
        a.dur = x.find("dur") ? seconds(*x.find("dur")) : 0.f;
        a.begin = x.find("begin") ? seconds(*x.find("begin")) : 0.f;
        if (const std::string* r = x.find("repeatCount")) a.repeat = (*r=="indefinite") ? -1.f : std::max(0.f, scalar(*r, 1));
        if (const std::string* f = x.find("fill")) a.freeze = (*f == "freeze");
        if (const std::string* m = x.find("calcMode")) { a.discrete = (*m=="discrete"); a.spline = (*m=="spline"); }
        a.additive = x.find("additive") && *x.find("additive") == "sum";

        std::vector<std::string> keys;
        if (const std::string* v = x.find("values")) split(*v, ';', keys);
        else if (const std::string* to = x.find("to")) { keys.push_back(x.find("from") ? *x.find("from") : std::string("0")); keys.push_back(*to); }
        else if (const std::string* by = x.find("by")) { keys.push_back("0"); keys.push_back(*by); a.additive = true; }
        if (x.name == "set" && x.find("to")) { keys.assign(1, *x.find("to")); a.discrete = true; a.freeze = true; if (a.dur<=0) a.dur = 1e6f; }
        if (keys.empty() || a.dur <= 0) return false;

        const std::string attribute = x.find("attributeName") ? *x.find("attributeName") : std::string();
        if (x.name == "animateTransform" || attribute == "transform") {
            a.target = TgtTransform;
            const std::string type = x.find("type") ? *x.find("type") : "translate";
            a.type = type=="scale" ? TrScale : type=="rotate" ? TrRotate : type=="skewX" ? TrSkewX : type=="skewY" ? TrSkewY : TrTranslate;
            a.stride = 3;
            for (const std::string& key : keys) {
                const std::vector<float> n = numbers(key);
                float v[3] = {0, 0, 0};
                if (a.type == TrScale) { v[0] = n.empty() ? 1.f : n[0]; v[1] = n.size()>1 ? n[1] : v[0]; }
                else { for (int i=0; i<3; ++i) v[i] = i < static_cast<int>(n.size()) ? n[i] : 0.f; }
                a.values.insert(a.values.end(), v, v+3);
            }
        } else if (attribute == "d") {
            a.target = TgtPathData; a.stride = 1;
            for (const std::string& key : keys) { a.paths.emplace_back(); parsePathData(key, a.paths.back()); }
            a.values.assign(keys.size(), 0.f);
        } else if (attribute == "fill" || attribute == "stroke") {
            a.target = attribute=="fill" ? TgtFill : TgtStroke; a.stride = 1;
            for (const std::string& key : keys) { Color c{0,0,0,0}; parsePaint(key, c); a.colors.push_back(c); }
            a.values.assign(keys.size(), 0.f);
        } else {
            if (attribute == "opacity") a.target = TgtOpacity;
            else if (attribute == "fill-opacity") a.target = TgtFillOpacity;
            else if (attribute == "stroke-opacity") a.target = TgtStrokeOpacity;
            else if (attribute == "stroke-width") a.target = TgtStrokeWidth;
            else if (geomSlot(owner.shape, attribute, a.slot)) { a.target = TgtGeom; owner.dynamic = true; }
            else { warn("SVG: unanimatable attribute: " + (attribute.empty() ? x.name : attribute)); return false; }
            a.stride = 1;
            for (const std::string& key : keys) a.values.push_back(scalar(key, 0));
        }

        const size_t count = keys.size();
        if (const std::string* kt = x.find("keyTimes")) {
            std::vector<std::string> parts;
            split(*kt, ';', parts);
            if (parts.size() == count) for (const std::string& part : parts) a.keyTimes.push_back(scalar(part, 0));
        }
        if (a.keyTimes.size() != count) {
            a.keyTimes.clear();
            for (size_t i=0; i<count; ++i) a.keyTimes.push_back(count<2 ? 0.f : static_cast<float>(i)/(count-1));
        }
        if (a.spline) {
            if (const std::string* ks = x.find("keySplines")) {
                std::vector<std::string> parts;
                split(*ks, ';', parts);
                for (const std::string& part : parts) {
                    const std::vector<float> n = numbers(part);
                    a.splines.push_back({n.size()>0?n[0]:0.f, n.size()>1?n[1]:0.f, n.size()>2?n[2]:1.f, n.size()>3?n[3]:1.f});
                }
            }
            if (a.splines.size() + 1 < count) a.spline = false;
        }
        return true;
    }

    // Decodes one <image> and resolves where its pixels land. The href must be
    // an embedded data: URI: an SVG compiled from a string has no base URL to
    // resolve a file or http reference against, and silently drawing nothing
    // for one is how a missing sprite turns into a mystery.
    bool buildImage(const XNode& x, Node& out) {
        const std::string* href = x.find("href");
        if (!href || href->empty()) { warn("SVG: <image> without an href"); return false; }
        const size_t marker = href->find(";base64,");
        if (href->compare(0, 5, "data:") != 0 || marker == std::string::npos) {
            warn("SVG: <image> href is not an embedded base64 data: URI");
            return false;
        }
        std::vector<std::uint8_t> bytes;
        if (!base64Decode(*href, marker + 8, bytes)) { warn("SVG: <image> has malformed base64"); return false; }
        auto raster = std::make_shared<Raster>();
        std::string error;
        if (!decodePng(bytes, *raster, error)) { warn("SVG: <image> could not be decoded: " + error); return false; }

        const float rw = static_cast<float>(raster->width), rh = static_cast<float>(raster->height);
        const float bx = x.number("x"), by = x.number("y");
        const float bw = x.find("width") ? x.number("width") : rw;
        const float bh = x.find("height") ? x.number("height") : rh;
        if (!(bw > 0 && bh > 0)) { warn("SVG: <image> has an empty box"); return false; }

        // preserveAspectRatio, resolved here rather than at draw time: it is a
        // property of the document, and the answer never changes per frame.
        unsigned char align = 4;   // xMidYMid
        bool meet = true;
        if (const std::string* par = x.find("preserveAspectRatio")) {
            const std::string v = trim(*par);
            if (v.find("none") != std::string::npos) align = 9;
            else {
                const size_t at = v.find('x');
                if (at != std::string::npos && at+7 < v.size()) {
                    const std::string ax = v.substr(at+1, 3), ay = v.substr(at+5, 3);
                    align = static_cast<unsigned char>((ax=="Min"?0:ax=="Max"?2:1)*3 + (ay=="Min"?0:ay=="Max"?2:1));
                }
            }
            meet = v.find("slice") == std::string::npos;
        }
        float sx = bw/rw, sy = bh/rh;
        if (align < 9) { const float s = meet ? std::min(sx, sy) : std::max(sx, sy); sx = sy = s; }
        static const float slot[3] = {0.f, 0.5f, 1.f};
        const float ox = align < 9 ? (bw - rw*sx) * slot[align/3] : 0.f;
        const float oy = align < 9 ? (bh - rh*sy) * slot[align%3] : 0.f;
        out.imageBox[0] = bx + ox; out.imageBox[1] = by + oy;
        out.imageBox[2] = rw*sx;   out.imageBox[3] = rh*sy;
        out.imageClip[0] = bx; out.imageClip[1] = by; out.imageClip[2] = bw; out.imageClip[3] = bh;
        out.imageSlice = !meet && align < 9;
        out.image = std::move(raster);
        return true;
    }

    bool build(const XNode& x, const Style& parent, Node& out, int depth) {
        if (depth > 40) { warn("SVG: tree too deep"); return false; }
        const std::string& tag = x.name;
        if (tag=="defs" || tag=="clipPath" || tag=="style" || tag=="title" || tag=="desc" || tag=="metadata" ||
            tag=="symbol" || tag=="mask" || tag=="marker" || tag=="filter" || tag=="pattern" || tag=="script") return false;
        if (tag=="linearGradient" || tag=="radialGradient" || tag=="text" || tag=="foreignObject") {
            warn("SVG: unsupported element <" + tag + ">");
            return false;
        }

        Props p;
        resolve(x, parent, p);
        if (!p.visible) return false;
        out.style = p.style;
        out.opacity = p.opacity;
        if (!p.transform.empty()) { out.transform = parseTransform(p.transform); out.transformed = true; }
        if (!p.origin.empty() && out.transformed) {
            const std::vector<float> o = numbers(p.origin);
            if (!o.empty()) {
                const float ox = o[0], oy = o.size()>1 ? o[1] : 0.f;
                out.transform = concat(concat(shift(ox, oy), out.transform), shift(-ox, -oy));
            }
        }
        if (!p.clipRef.empty()) out.clip = clipFor(p.clipRef);

        bool container = false;
        if (tag=="g" || tag=="svg" || tag=="a" || tag=="switch") container = true;
        else if (tag=="rect") { out.shape=ShapeRect; out.geom[0]=x.number("x"); out.geom[1]=x.number("y"); out.geom[2]=x.number("width"); out.geom[3]=x.number("height"); out.geom[4]=x.find("rx")?x.number("rx"):-1.f; out.geom[5]=x.find("ry")?x.number("ry"):-1.f; }
        else if (tag=="circle") { out.shape=ShapeCircle; out.geom[0]=x.number("cx"); out.geom[1]=x.number("cy"); out.geom[2]=x.number("r"); }
        else if (tag=="ellipse") { out.shape=ShapeEllipse; out.geom[0]=x.number("cx"); out.geom[1]=x.number("cy"); out.geom[2]=x.number("rx"); out.geom[3]=x.number("ry"); }
        else if (tag=="line") { out.shape=ShapeLine; out.geom[0]=x.number("x1"); out.geom[1]=x.number("y1"); out.geom[2]=x.number("x2"); out.geom[3]=x.number("y2"); }
        else if (tag=="polygon" || tag=="polyline") { out.shape = tag=="polygon" ? ShapePolygon : ShapePolyline; if (const std::string* v = x.find("points")) numbersInto(*v, out.points); }
        else if (tag=="path") { out.shape=ShapePath; if (const std::string* d = x.find("d")) { if (parsePathData(*d, out.path) > 0) warn("SVG: malformed path data"); } }
        else if (tag=="image") { if (!buildImage(x, out)) return false; }
        else if (tag=="use") {
            const std::string* href = x.find("href");
            const size_t hash = href ? href->find('#') : std::string::npos;
            if (hash == std::string::npos) { warn("SVG: <use> without a fragment reference"); return false; }
            auto it = ids.find(href->substr(hash+1));
            if (it == ids.end()) { warn("SVG: <use> target not found: " + *href); return false; }
            const float ux = x.number("x"), uy = x.number("y");
            if (ux != 0 || uy != 0) { out.transform = concat(out.transform, shift(ux, uy)); out.transformed = true; }
            out.kids.emplace_back();
            if (!build(*it->second, out.style, out.kids.back(), depth+1)) out.kids.pop_back();
        } else { warn("SVG: unsupported element <" + tag + ">"); return false; }

        if (out.shape != ShapeNone && out.shape != ShapePath) buildShape(out.shape, out.geom, out.points, out.path);

        for (const XNode& kid : x.kids) {
            if (kid.name=="animate" || kid.name=="animateTransform" || kid.name=="set") {
                Anim a;
                if (parseAnim(kid, out, a)) out.anims.push_back(std::move(a));
                continue;
            }
            if (kid.name=="animateMotion") { warn("SVG: <animateMotion> is not supported"); continue; }
            if (!container) continue;
            out.kids.emplace_back();
            if (!build(kid, out.style, out.kids.back(), depth+1)) out.kids.pop_back();
        }
        return out.shape != ShapeNone || out.image || !out.kids.empty() || !out.anims.empty();
    }
};

// --- bounds (only to decide whether a viewport clip is needed at all) --------
struct Box { float x0=1e30f, y0=1e30f, x1=-1e30f, y1=-1e30f; bool valid() const { return x1>=x0; } };
void addPoint(Box& b, const Mat& m, float x, float y) {
    const float px = m[0]*x + m[2]*y + m[4], py = m[1]*x + m[3]*y + m[5];
    if (!std::isfinite(px) || !std::isfinite(py)) return;
    b.x0 = std::min(b.x0, px); b.x1 = std::max(b.x1, px);
    b.y0 = std::min(b.y0, py); b.y1 = std::max(b.y1, py);
}
void measure(const Node& n, const Mat& parent, Box& box) {
    const Mat m = n.transformed ? concat(parent, n.transform) : parent;
    const float pad = n.style.strokeOn ? n.style.strokeWidth/2 : 0.f;
    for (const Path2D::Segment& s : n.path.segments()) {
        const float* v = s.v;
        switch (s.command) {
        case Path2D::Command::Move: case Path2D::Command::Line: addPoint(box, m, v[0]-pad, v[1]-pad); addPoint(box, m, v[0]+pad, v[1]+pad); break;
        case Path2D::Command::Quadratic: for (int k=0;k<2;++k) { addPoint(box, m, v[k*2]-pad, v[k*2+1]-pad); addPoint(box, m, v[k*2]+pad, v[k*2+1]+pad); } break;
        case Path2D::Command::Bezier: for (int k=0;k<3;++k) { addPoint(box, m, v[k*2]-pad, v[k*2+1]-pad); addPoint(box, m, v[k*2]+pad, v[k*2+1]+pad); } break;
        case Path2D::Command::Arc: { const float r=std::abs(v[2])+pad; addPoint(box,m,v[0]-r,v[1]-r); addPoint(box,m,v[0]+r,v[1]+r); break; }
        case Path2D::Command::Ellipse: { const float r=std::max(std::abs(v[2]),std::abs(v[3]))+pad; addPoint(box,m,v[0]-r,v[1]-r); addPoint(box,m,v[0]+r,v[1]+r); break; }
        case Path2D::Command::Rect: case Path2D::Command::RoundRect:
            addPoint(box, m, v[0]-pad, v[1]-pad); addPoint(box, m, v[0]+v[2]+pad, v[1]+v[3]+pad); break;
        default: break;
        }
    }
    if (n.image) {
        const float* b = n.imageSlice ? n.imageClip : n.imageBox;
        addPoint(box, m, b[0], b[1]); addPoint(box, m, b[0]+b[2], b[1]+b[3]);
    }
    for (const Node& kid : n.kids) measure(kid, m, box);
}

} // namespace svgc

struct SvgDocument::Scene {
    svgc::Node root;
    std::vector<svgc::Clip> clips;
    bool spills = false;
    bool animated = false;
};

namespace svgc {

Path2D gShape;         // one reusable scratch buffer keeps render() allocation-free
Path2D gViewportBox;   // the same, for the viewport clip rect
Path2D gImageClip;     // and for a sliced <image>'s own box

void drawNode(const Node& n, const std::vector<Clip>& clips, Canvas& canvas, float time, float alpha) {
    Mat m = n.transform;
    bool transformed = n.transformed;
    float opacity = n.opacity;
    for (const Anim& a : n.anims) {
        int key; float u;
        if (a.target == TgtTransform) {
            if (!animPhase(a, time, key, u)) continue;
            float v[3]; animValues(a, key, u, v);
            const Mat t = transformOf(a.type, v);
            m = a.additive ? concat(m, t) : t;
            transformed = true;
        } else if (a.target == TgtOpacity) {
            if (animPhase(a, time, key, u)) { float v[1]; animValues(a, key, u, v); opacity = a.additive ? opacity*v[0] : v[0]; }
        }
    }
    alpha *= std::min(1.f, std::max(0.f, opacity));
    if (alpha <= 0.002f) return;

    // A node's scope is only ever undoing three things: a transform, a clip,
    // and a dash. Everything else it touches -- fill, stroke, width, cap, join,
    // miter -- is written again by the next node that uses it, and the document
    // as a whole is already inside renderFitted's own save, so nothing leaks
    // past the picture. A leaf with none of the three therefore needs no scope
    // at all, and most leaves are exactly that: on the title screen this was
    // ~240 save/restore pairs a frame, and in the browser each pair is two
    // calls out of wasm and two pushes of the context's state machine.
    //
    // The dash is in the list because it is the one style a node sets
    // conditionally (`if (!dash.empty())`), so an undashed stroke after a
    // dashed one does NOT rewrite it. Keeping the scope whenever this node
    // declares a dash is what contains that, and it composes: a descendant
    // that declares one keeps its own scope inside this one.
    const bool clipped = n.clip >= 0 && n.clip < static_cast<int>(clips.size());
    const bool scoped = transformed || clipped || !n.style.dash.empty();
    if (scoped) canvas.save();
    if (transformed) canvas.transform(m[0], m[1], m[2], m[3], m[4], m[5]);
    if (clipped) {
        const Clip& clip = clips[n.clip];
        canvas.clip(clip.path, clip.evenOdd ? "evenodd" : "nonzero");
    }

    if (n.shape != ShapeNone) {
        const Path2D* path = &n.path;
        Color fill = n.style.fill, stroke = n.style.stroke;
        float fillOpacity = n.style.fillOpacity, strokeOpacity = n.style.strokeOpacity, width = n.style.strokeWidth;
        if (n.dynamic) {
            float g[6] = {n.geom[0], n.geom[1], n.geom[2], n.geom[3], n.geom[4], n.geom[5]};
            for (const Anim& a : n.anims) {
                int key; float u;
                if (a.target != TgtGeom || !animPhase(a, time, key, u)) continue;
                float v[1]; animValues(a, key, u, v);
                g[a.slot] = a.additive ? g[a.slot] + v[0] : v[0];
            }
            buildShape(n.shape, g, n.points, gShape);
            path = &gShape;
        }
        for (const Anim& a : n.anims) {
            int key; float u;
            if (a.target == TgtNone || a.target == TgtGeom || a.target == TgtTransform || a.target == TgtOpacity) continue;
            if (!animPhase(a, time, key, u)) continue;
            if (a.target == TgtPathData && !a.paths.empty()) {
                const Path2D& p0 = a.paths[std::min<size_t>(key, a.paths.size()-1)];
                const Path2D& p1 = a.paths[std::min<size_t>(key+1, a.paths.size()-1)];
                if (p0.segments().size() == p1.segments().size()) {
                    gShape.segments() = p0.segments();
                    std::vector<Path2D::Segment>& dst = gShape.segments();
                    for (size_t i=0; i<dst.size(); ++i) {
                        if (dst[i].command != p1.segments()[i].command) continue;
                        for (int k=0; k<8; ++k) dst[i].v[k] = p0.segments()[i].v[k] + (p1.segments()[i].v[k] - p0.segments()[i].v[k])*u;
                    }
                    path = &gShape;
                } else path = &p0;
            } else if (a.target == TgtFill && !a.colors.empty()) {
                fill = mixColor(a.colors[std::min<size_t>(key, a.colors.size()-1)], a.colors[std::min<size_t>(key+1, a.colors.size()-1)], u);
            } else if (a.target == TgtStroke && !a.colors.empty()) {
                stroke = mixColor(a.colors[std::min<size_t>(key, a.colors.size()-1)], a.colors[std::min<size_t>(key+1, a.colors.size()-1)], u);
            } else {
                float v[1]; animValues(a, key, u, v);
                if (a.target == TgtFillOpacity) fillOpacity = v[0];
                else if (a.target == TgtStrokeOpacity) strokeOpacity = v[0];
                else if (a.target == TgtStrokeWidth) width = std::max(0.f, v[0]);
            }
        }
        if (!path->empty()) {
            if (n.style.fillOn) {
                canvas.setFillStyle(faded(fill, alpha*fillOpacity));
                canvas.fill(*path, n.style.fillEvenOdd ? "evenodd" : "nonzero");
            }
            if (n.style.strokeOn && width > 0) {
                canvas.setStrokeStyle(faded(stroke, alpha*strokeOpacity));
                canvas.setLineWidth(width);
                canvas.setLineCap(kCapName[n.style.cap]);
                canvas.setLineJoin(kJoinName[n.style.join]);
                canvas.setMiterLimit(n.style.miterLimit);
                if (!n.style.dash.empty()) { canvas.setLineDash(n.style.dash); canvas.setLineDashOffset(n.style.dashOffset); }
                canvas.stroke(*path);
            }
        }
    }
    if (n.image && n.image->width > 0) {
        // 'slice' is the one case that draws outside its own box, so it is the
        // one case that needs a clip; 'meet' always fits inside.
        if (n.imageSlice) {
            canvas.save();
            gImageClip.clear();
            gImageClip.rect(n.imageClip[0], n.imageClip[1], n.imageClip[2], n.imageClip[3]);
            canvas.clip(gImageClip, "nonzero");
        }
        canvas.drawImage(n.image->rgba.data(), n.image->width, n.image->height,
                         n.imageBox[0], n.imageBox[1], n.imageBox[2], n.imageBox[3], alpha);
        if (n.imageSlice) canvas.restore();
    }
    for (const Node& kid : n.kids) drawNode(kid, clips, canvas, time, alpha);
    if (scoped) canvas.restore();
}

Mat viewportMatrix(float vx, float vy, float vw, float vh, unsigned char align, unsigned char meet,
                   float x, float y, float w, float h) {
    float sx = vw > 0 ? w/vw : 1.f, sy = vh > 0 ? h/vh : 1.f;
    if (align < 9) { const float s = meet ? std::min(sx, sy) : std::max(sx, sy); sx = sy = s; }
    float tx = x - vx*sx, ty = y - vy*sy;
    if (align < 9) {
        static const float slot[3] = {0.f, 0.5f, 1.f};
        tx += (w - vw*sx) * slot[align/3];
        ty += (h - vh*sy) * slot[align%3];
    }
    return Mat{sx, 0, 0, sy, tx, ty};
}

} // namespace svgc

using namespace svgc;

SvgDocument::~SvgDocument() = default;

bool SvgDocument::empty() const { return !scene_ || (scene_->root.kids.empty() && scene_->root.shape == ShapeNone); }

SvgDocument SvgDocument::fromFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { SvgDocument document; document.warnings_.push_back("SVG: cannot open " + path); return document; }
    std::stringstream source; source << input.rdbuf();
    return fromString(source.str());
}

SvgDocument SvgDocument::fromString(const std::string& source) {
    SvgDocument document;
    XNode root;
    if (!parseXml(source, root, document.warnings_)) {
        document.warnings_.push_back("SVG: no elements found");
        return document;
    }

    Scene scene;
    Builder builder{scene.clips, document.warnings_, {}, {}, {}};
    builder.collect(root, 0);

    if (const std::string* box = root.find("viewBox")) {
        const std::vector<float> v = numbers(*box);
        if (v.size() >= 4 && v[2] > 0 && v[3] > 0) {
            document.viewX_ = v[0]; document.viewY_ = v[1];
            document.viewW_ = v[2]; document.viewH_ = v[3];
            document.hasViewBox_ = true;
        } else document.warnings_.push_back("SVG: malformed viewBox");
    }
    const std::string* w = root.find("width");
    const std::string* h = root.find("height");
    document.width_ = w && w->find('%') == std::string::npos ? scalar(*w, 0) : 0;
    document.height_ = h && h->find('%') == std::string::npos ? scalar(*h, 0) : 0;
    if (document.width_ <= 0) document.width_ = document.hasViewBox_ ? document.viewW_ : 100.f;
    if (document.height_ <= 0) document.height_ = document.hasViewBox_ ? document.viewH_ : 100.f;
    if (!document.hasViewBox_) { document.viewX_ = document.viewY_ = 0; document.viewW_ = document.width_; document.viewH_ = document.height_; }

    if (const std::string* par = root.find("preserveAspectRatio")) {
        const std::string v = trim(*par);
        if (v.find("none") != std::string::npos) document.align_ = 9;
        else {
            const size_t x = v.find('x');
            if (x != std::string::npos && x+7 < v.size()) {
                const std::string ax = v.substr(x+1, 3), ay = v.substr(x+5, 3);
                document.align_ = static_cast<unsigned char>((ax=="Min"?0:ax=="Max"?2:1)*3 + (ay=="Min"?0:ay=="Max"?2:1));
            }
        }
        document.meet_ = v.find("slice") == std::string::npos ? 1 : 0;
    }

    builder.build(root, Style{}, scene.root, 0);
    scene.root.transformed = false;          // the root transform is the viewport mapping
    scene.root.transform = kUnit;

    // Whether a clip is needed at all, asked of the viewBox: the viewport that
    // actually clips is at least as large, so this over-answers at worst, and
    // the cost of a clip nothing reaches is one intersected mask.
    Box bounds;
    measure(scene.root, kUnit, bounds);
    const float slack = 0.01f * std::max(document.viewW_, document.viewH_);
    // Asked once, at build time: whether anything under the root animates.
    // `render` walks the same tree per frame anyway, but callers that want to
    // cache a rasterisation have to know this before they draw, not after.
    const auto anyAnim = [](auto&& self, const svgc::Node& n) -> bool {
        if (!n.anims.empty()) return true;
        for (const svgc::Node& kid : n.kids) if (self(self, kid)) return true;
        return false;
    };
    scene.animated = anyAnim(anyAnim, scene.root);

    scene.spills = bounds.valid() &&
                   (bounds.x0 < document.viewX_ - slack || bounds.y0 < document.viewY_ - slack ||
                    bounds.x1 > document.viewX_ + document.viewW_ + slack || bounds.y1 > document.viewY_ + document.viewH_ + slack);

    document.scene_ = std::make_shared<const Scene>(std::move(scene));
    return document;
}

bool SvgDocument::animated() const { return scene_ && scene_->animated; }

bool SvgDocument::render(Canvas& canvas, float time) const {
    if (!scene_) return false;
    // Without a viewBox the document's own coordinates are already canvas
    // coordinates, which is what callers of the pre-viewBox library expect.
    if (!hasViewBox_) { drawNode(scene_->root, scene_->clips, canvas, time, 1.f); return true; }
    return renderFitted(canvas, 0, 0, width_, height_, time);
}

bool SvgDocument::renderFitted(Canvas& canvas, float x, float y, float size, float time) const {
    return renderFitted(canvas, x, y, size, size, time);
}

bool SvgDocument::renderFitted(Canvas& canvas, float x, float y, float width, float height, float time) const {
    if (!scene_ || viewW_ <= 0 || viewH_ <= 0) return false;
    const Mat m = viewportMatrix(viewX_, viewY_, viewW_, viewH_, align_, meet_, x, y, width, height);
    canvas.save();
    // The SVG viewport, not the viewBox, is what clips: the viewBox is only a
    // transform, and with 'meet' the viewport is the LARGER of the two. Cutting
    // at the viewBox instead showed only the letterbox band -- dust and glitch
    // draw a 32x32 <image> through a 32x10 viewBox, and the browser paints all
    // of it. Clipping here, before the viewBox transform goes on, is exactly
    // the target box the caller asked to fill.
    if (scene_->spills) {
        gViewportBox.clear();
        gViewportBox.rect(x, y, width, height);
        canvas.clip(gViewportBox, "nonzero");
    }
    canvas.transform(m[0], m[1], m[2], m[3], m[4], m[5]);
    drawNode(scene_->root, scene_->clips, canvas, time, 1.f);
    canvas.restore();
    return true;
}
