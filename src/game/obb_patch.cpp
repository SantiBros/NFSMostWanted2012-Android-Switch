#include "game/obb_patch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/common.h"
#include "core/crc32.h"
#include "game/setup.h"
#include "hle/file_cache.h"

namespace game::obb_patch {

namespace {

u16 rd16(const std::string& s, size_t o) { return (u16)((u8)s[o] | (u8)s[o + 1] << 8); }
u32 rd32(const std::string& s, size_t o) { return (u32)rd16(s, o) | (u32)rd16(s, o + 2) << 16; }
void wr16(std::string& s, size_t o, u16 v) { s[o] = (char)(v & 0xFF), s[o + 1] = (char)(v >> 8); }
void wr32(std::string& s, size_t o, u32 v) { wr16(s, o, (u16)v), wr16(s, o + 2, (u16)(v >> 16)); }
void put16(std::string& s, u16 v) { s.push_back((char)(v & 0xFF)), s.push_back((char)(v >> 8)); }
void put32(std::string& s, u32 v) { put16(s, (u16)v), put16(s, (u16)(v >> 16)); }

// FNV-1 (32-bit), the hash of each SBIN chunk.
u32 fnv1(const std::string& s) {
    u32 h = 0x811C9DC5u;
    for (unsigned char c : s) h = (h * 0x01000193u) ^ c;
    return h;
}

// ---- texts ---------------------------------------------------------------------
bool is_key(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// Lower-case letters anywhere (Latin or Cyrillic), or a script without case
// (CJK, Hangul, kana): then "Switch", else "SWITCH" (a title in capitals).
bool mixed_case(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        u32 c = (u8)s[i];
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n == 2 && i + 1 < s.size()) c = (c & 0x1F) << 6 | ((u8)s[i + 1] & 0x3F);
        else if (n == 3 && i + 2 < s.size()) c = (c & 0x0F) << 12 | ((u8)s[i + 1] & 0x3F) << 6 | ((u8)s[i + 2] & 0x3F);
        i += n;
        if ((c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF) || (c >= 0x430 && c <= 0x45F)) return true;
        if (c >= 0x2E80) return true;  // CJK, kana, Hangul
    }
    return false;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
}

// The new text for a string pair, or the old one.
std::string new_text(const std::string& key, const std::string& value) {
    // The button labels of the controls pages.
    if (key == "UI_MOGA_START") return "+";
    if (key == "UI_MOGA_SELECT") return "-";
    std::string v = value;
    // "Tap the Start button to" PAUSE: the Switch's pause button is +.
    if (key == "TUTORIAL_MOGA_PAUSE_DESCRIPTION")
        for (const char* start : {"START", "Start", "\xE5\xBC\x80\xE5\xA7\x8B" /* 开始 */, "\xEC\x8B\x9C\xEC\x9E\x91" /* 시작 */}) {
            const size_t p = v.find(start);
            if (p != std::string::npos) {
                v.replace(p, std::strlen(start), "+");
                break;
            }
        }
    if (v.find("MOGA") != std::string::npos) replace_all(v, "MOGA", mixed_case(v) ? "Switch" : "SWITCH");
    return v;
}

// The menu controls page (Options > Controls) has a callout line for each face
// button, placed for the MOGA's arrangement (Y top, X left, B right, A
// bottom). The port's picture has the Switch's (X top, Y left, A right, B
// bottom), so the texts at the ends of those lines trade places: the line to
// the top button now says what X does, and so on.
const std::pair<const char*, const char*> kSwapped[] = {
    {"MOGA_CONTROLS_SHOP", "MOGA_CONTROLS_MOST_WANTED"},
    {"MOGA_CONTROLS_BACK", "MOGA_CONTROLS_CONFIRM"},
};

// SBIN chunks: tag, size, FNV-1 hash, data.
struct Chunk {
    std::string tag, data;
};
// Each chunk starts at a multiple of 4 bytes (zero padding before it if
// needed).
bool split_chunks(const std::string& sbin, std::vector<Chunk>& chunks) {
    if (sbin.size() < 8 || sbin.compare(0, 4, "SBIN") != 0) return false;
    for (size_t p = 8; p + 12 <= sbin.size();) {
        const u32 size = rd32(sbin, p + 4);
        if (p + 12 + size > sbin.size()) return false;
        chunks.push_back({sbin.substr(p, 4), sbin.substr(p + 12, size)});
        p += 12 + size;
        while (p % 4 && p < sbin.size() && sbin[p] == 0) p++;
    }
    return true;
}
std::string join_chunks(const std::string& sbin, const std::vector<Chunk>& chunks) {
    std::string out = sbin.substr(0, 8);
    for (const Chunk& c : chunks) {
        while (out.size() % 4) out.push_back('\0');
        out += c.tag;
        put32(out, (u32)c.data.size());
        put32(out, fnv1(c.data));
        out += c.data;
    }
    return out;
}

}  // namespace

std::string patch_string_table(const std::string& sbin, int* changed) {
    if (changed) *changed = 0;
    std::vector<Chunk> chunks;
    if (!split_chunks(sbin, chunks)) return sbin;
    Chunk *chdr = nullptr, *cdat = nullptr, *data = nullptr;
    for (Chunk& c : chunks) {
        if (c.tag == "CHDR") chdr = &c;
        if (c.tag == "CDAT") cdat = &c;
        if (c.tag == "DATA") data = &c;
    }
    if (!chdr || !cdat || !data || chdr->data.size() % 8) return sbin;
    const size_t n = chdr->data.size() / 8;
    std::vector<std::string> strs(n);
    for (size_t i = 0; i < n; i++) {
        const u32 off = rd32(chdr->data, i * 8), len = rd32(chdr->data, i * 8 + 4);
        if ((u64)off + len > cdat->data.size()) return sbin;
        strs[i] = cdat->data.substr(off, len);
    }
    // StringPair records in DATA: string ids of the key and of the text,
    // then 0xFFFFFFFF. Texts are shared: "START" is the text of UI_MOGA_START
    // and of the race's HUD_INGAME_START, "CONFIRM" of a MOGA key and of the
    // age gate's button. So a change never touches a shared string: the new
    // text is added to the table and only that key's record points at it.
    struct Record {
        size_t at;
        u16 key, text;
    };
    std::vector<Record> records;
    std::unordered_map<std::string, std::string> text_of;  // key -> its original text
    for (size_t p = 0; p + 8 <= data->data.size(); p += 4) {
        if (rd32(data->data, p + 4) != 0xFFFFFFFFu) continue;
        const u16 k = rd16(data->data, p), v = rd16(data->data, p + 2);
        if (k < n && v < n && is_key(strs[k])) {
            records.push_back({p, k, v});
            text_of.emplace(strs[k], strs[v]);
        }
    }
    std::unordered_map<std::string, std::string> swapped;  // key -> the text it shows now
    for (const auto& [k1, k2] : kSwapped)
        if (text_of.count(k1) && text_of.count(k2)) {
            swapped[k1] = text_of[k2];
            swapped[k2] = text_of[k1];
        }
    std::unordered_map<std::string, u16> added;  // new text -> its string id
    u32 next_id = (u32)n;
    int count = 0;
    for (const Record& r : records) {
        const std::string& key = strs[r.key];
        const auto sw = swapped.find(key);
        const std::string text = sw != swapped.end() ? sw->second : new_text(key, strs[r.text]);
        if (text == strs[r.text]) continue;
        auto it = added.find(text);
        if (it == added.end()) {
            if (next_id > 0xFFFF) break;  // string ids are 16-bit
            put32(chdr->data, (u32)cdat->data.size());
            put32(chdr->data, (u32)text.size());
            cdat->data += text;
            cdat->data.push_back('\0');
            it = added.emplace(text, (u16)next_id++).first;
        }
        wr16(data->data, r.at + 2, it->second);
        count++;
        static const bool dump = std::getenv("NFSMW_OBB_PATCH_DUMP") != nullptr;  // debugging: every change
        if (dump) LOGD("OBB text: %s: \"%s\" -> \"%s\"", key.c_str(), strs[r.text].c_str(), text.c_str());
    }
    if (changed) *changed = count;
    if (!count) return sbin;
    return join_chunks(sbin, chunks);
}

namespace {

// One field of one object in the layouts file's DATA chunk: the object's
// index (its place in OHDR), the byte offset in the object, the size (2 or 4)
// and the value it has in 1.3.128 and the new one. Floats as their bits.
struct Edit {
    u32 object, offset, size, from, to;
};
u32 bits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}
// Entities: type (u16), entity id (u16 + 2), then the rectangle: top-left x,
// y, anchor x, anchor y, bottom-right x, y, anchor x, anchor y (floats; layout
// units around the page's centre).
constexpr u32 kTlX = 6, kTlY = 10, kBrX = 22, kBrY = 26;
void move_rect(std::vector<Edit>& e, u32 obj, float x0, float y0, float x1, float y1, float nx0, float ny0,
               float nx1, float ny1) {
    e.push_back({obj, kTlX, 4, bits(x0), bits(nx0)});
    e.push_back({obj, kTlY, 4, bits(y0), bits(ny0)});
    e.push_back({obj, kBrX, 4, bits(x1), bits(nx1)});
    e.push_back({obj, kBrY, 4, bits(y1), bits(ny1)});
}
void shift_y(std::vector<Edit>& e, u32 obj, float x0, float y0, float x1, float y1, float dy) {
    move_rect(e, obj, x0, y0, x1, y1, x0, y0 + dy, x1, y1 + dy);
}

// The changes to the two controller pages, so that every callout line ends on
// its button in the port's picture (game/controller_art.h): the Switch's
// arrangement, with - and + at the top.
std::vector<Edit> layout_edits() {
    std::vector<Edit> e;
    constexpr u32 kHalignCentre = 1, kHalignRight = 2;
    constexpr u32 kGuideLeft = 1438, kGuideLeft2 = 1441;  // string ids: options_help_guide_left / _left2
    // Game controls page (options_help_moga_game).
    // Drift: the line ends on A, now right of the cluster.
    move_rect(e, 14723, 70, -6, 203, 14, 95.5f, -6, 203, 14);
    // Pause: "Tap the + button to" and PAUSE above the picture, its line down
    // to + at the top (a line that ends at its lower right).
    move_rect(e, 14705, -203, 74, -52, 94, -35, -53.1f, 27.3f, -33.1f);
    e.push_back({14706, 2, 2, kGuideLeft2, kGuideLeft});
    move_rect(e, 14668, -226, 53, -144, 88, -54, -80, 34, -70);
    e.push_back({14669, 2, 4, kHalignRight, kHalignCentre});
    move_rect(e, 14663, -145, 96, -145, 96, -10, -67, -10, -67);
    e.push_back({14664, 2, 4, kHalignRight, kHalignCentre});
    // Menu controls page (options_help_moga_menu): everything 20 units lower,
    // where the game controls page has its picture, so that there is room
    // above it.
    shift_y(e, 14752, -141, -79, 142, 89, 20);   // the picture
    shift_y(e, 14762, 158, -75, 215, -55, 20);   // MENU (right)
    shift_y(e, 14772, 142, -49, 203, -30, 20);   // SHOP (the top button's line)
    shift_y(e, 14777, -170, -75, -170, -75, 20); // MENU (left)
    shift_y(e, 14782, 54, 95, 54, 94, 20);       // MOST WANTED
    shift_y(e, 14787, 142, 94, 142, 94, 20);     // CONFIRM
    shift_y(e, 14792, -133, -39, -133, -39, 20); // NAVIGATION
    shift_y(e, 14797, 147, 18, 208, 37, 20);     // BACK
    shift_y(e, 14802, -217, -81, -58, -61, 20);  // their lines
    shift_y(e, 14808, -228, -47, -74, -27, 20);
    shift_y(e, 14814, -12, 31, 87, 51, 20);
    shift_y(e, 14832, 55, -83, 206, -63, 20);
    shift_y(e, 14838, 70, -55, 203, -35, 20);
    shift_y(e, 14844, 47, 38, 132, 58, 20);
    shift_y(e, 14850, 90, -5, 207, 15, 20);
    // ORIGIN (- , the EA Origin account: offline in the port): off the page.
    move_rect(e, 14767, -161, 43, -161, 43, -4000, 43, -4000, 43);
    move_rect(e, 14820, -214, 21, -52, 41, -4000, 21, -3900, 41);
    // EASYDRIVE (+): above the picture, its line down to +, as on the game page.
    move_rect(e, 14826, -214, 53, -52, 73, -35, -53.1f, 27.3f, -33.1f);
    e.push_back({14827, 2, 2, kGuideLeft2, kGuideLeft});
    move_rect(e, 14757, -129, 76, -129, 76, -12, -66, -12, -66);
    e.push_back({14758, 2, 4, kHalignRight, kHalignCentre});
    // The map's EasyDrive button: its label, now "+" instead of "START", in
    // the larger font and moved right, to the middle of the round button
    // ("START" filled it from its left edge).
    constexpr u32 kFontBlack8 = 321, kFontBlack12 = 146;  // string ids: mw_black_8 / _12
    move_rect(e, 1855, 6, 18, 39, 28, 19, 18, 52, 28);
    e.push_back({1856, 10, 2, kFontBlack8, kFontBlack12});
    return e;
}

}  // namespace

std::string patch_layouts(const std::string& sbin, int* changed) {
    if (changed) *changed = 0;
    std::vector<Chunk> chunks;
    if (!split_chunks(sbin, chunks)) return sbin;
    Chunk *data = nullptr, *ohdr = nullptr;
    for (Chunk& c : chunks) {
        if (c.tag == "DATA") data = &c;
        if (c.tag == "OHDR") ohdr = &c;
    }
    if (!data || !ohdr) return sbin;
    // OHDR: each object's start in DATA, in bits.
    const std::vector<Edit> edits = layout_edits();
    auto where = [&](const Edit& ed, size_t& at) {
        if ((u64)ed.object * 4 + 4 > ohdr->data.size()) return false;
        at = rd32(ohdr->data, ed.object * 4) / 8 + ed.offset;
        return at + ed.size <= data->data.size();
    };
    for (const Edit& ed : edits) {  // all or nothing: a different file stays as it is
        size_t at = 0;
        if (!where(ed, at)) return sbin;
        const u32 now = ed.size == 2 ? rd16(data->data, at) : rd32(data->data, at);
        if (now != ed.from) {
            LOGW("OBB: the controls pages' layout is not the one the port knows (object %u); left as it is",
                 ed.object);
            return sbin;
        }
    }
    for (const Edit& ed : edits) {
        size_t at = 0;
        where(ed, at);
        if (ed.size == 2) wr16(data->data, at, (u16)ed.to);
        else wr32(data->data, at, ed.to);
    }
    if (changed) *changed = 1;
    return join_chunks(sbin, chunks);
}

namespace {

bool read_at(FILE* f, u64 off, size_t n, std::string& out) {
    out.resize(n);
    return fseeko(f, (off_t)off, SEEK_SET) == 0 && std::fread(out.data(), 1, n, f) == n;
}

// The bytes to append to the OBB: the changed entries, stored uncompressed,
// then the whole zip directory again (pointing at them) and its end record.
std::string build_tail(const std::string& path) {
    const auto t0 = std::chrono::steady_clock::now();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string tail;
    do {
        if (fseeko(f, 0, SEEK_END) != 0) break;
        const u64 size = (u64)ftello(f);
        const u64 look = std::min<u64>(size, 0xFFFF + 22);
        std::string end;
        if (!read_at(f, size - look, (size_t)look, end)) break;
        size_t e = std::string::npos;
        for (size_t p = end.size() - 22 + 1; p-- > 0;)
            if (rd32(end, p) == 0x06054B50u) {
                e = p;
                break;
            }
        if (e == std::string::npos) break;
        std::string eocd = end.substr(e);
        const u32 cd_size = rd32(eocd, 12), cd_off = rd32(eocd, 16);
        std::string cd;
        if ((u64)cd_off + cd_size > size || !read_at(f, cd_off, cd_size, cd)) break;
        std::string entries;  // the changed entries (local header + data)
        int files = 0, texts = 0, tables = 0, layout_lines = 0;
        for (size_t p = 0; p + 46 <= cd.size() && rd32(cd, p) == 0x02014B50u;) {
            const u16 method = rd16(cd, p + 10), nlen = rd16(cd, p + 28), xlen = rd16(cd, p + 30), clen = rd16(cd, p + 32);
            const u32 csize = rd32(cd, p + 20), usize = rd32(cd, p + 24), local = rd32(cd, p + 42);
            const std::string name = cd.substr(p + 46, nlen);
            const size_t next = p + 46 + nlen + xlen + clen;
            const bool strings =
                name.rfind("published/stringdata/", 0) == 0 && name.find("/nfsmw_android.sb") != std::string::npos;
            const bool layouts = name == "published/layouts/layouts.sb";
            if ((strings || layouts) && (method == 0 || method == 8) && usize < (16u << 20)) {
                std::string lh, comp;
                if (read_at(f, local, 30, lh) && rd32(lh, 0) == 0x04034B50u &&
                    read_at(f, (u64)local + 30 + rd16(lh, 26) + rd16(lh, 28), csize, comp)) {
                    std::string plain;
                    if (method == 0) plain = comp;
                    else {
                        plain.resize(usize);
                        if (!setup::inflate_raw(comp.data(), comp.size(), plain.data(), plain.size())) plain.clear();
                    }
                    int n = 0;
                    const std::string patched = plain.empty() ? plain
                                                : strings     ? patch_string_table(plain, &n)
                                                              : patch_layouts(plain, &n);
                    if (n > 0) {
                        const u32 crc = checksum::crc32(0, patched.data(), patched.size());
                        const u32 at = (u32)(size + entries.size());
                        // Local header: stored, no data descriptor.
                        std::string h;
                        put32(h, 0x04034B50u);
                        put16(h, 10);                                // version needed
                        put16(h, (u16)(rd16(cd, p + 8) & 0x0800));   // flags: keep "UTF-8 names" only
                        put16(h, 0);                                 // stored
                        put16(h, rd16(cd, p + 12)), put16(h, rd16(cd, p + 14));  // time, date
                        put32(h, crc), put32(h, (u32)patched.size()), put32(h, (u32)patched.size());
                        put16(h, nlen), put16(h, 0);
                        entries += h + name + patched;
                        // The directory record now points at it.
                        wr16(cd, p + 8, (u16)(rd16(cd, p + 8) & 0x0800));
                        wr16(cd, p + 10, 0);
                        wr32(cd, p + 16, crc);
                        wr32(cd, p + 20, (u32)patched.size());
                        wr32(cd, p + 24, (u32)patched.size());
                        wr32(cd, p + 42, at);
                        files++;
                        if (strings) texts += n, tables++;
                        else layout_lines += n;
                    }
                }
            }
            p = next;
        }
        if (!files) break;
        if (size + entries.size() + cd.size() + eocd.size() > 0xFFFFFFFFull) break;  // no zip64
        wr32(eocd, 12, (u32)cd.size());
        wr32(eocd, 16, (u32)(size + entries.size()));
        tail = entries + cd + eocd;
        LOGI("OBB: %d string tables with %d texts changed for the Switch (MOGA -> Switch), controller pages' layout "
             "%s, %zu KiB added in memory, %.0f ms",
             tables, texts, layout_lines ? "changed" : "left as it is", tail.size() / 1024,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    } while (false);
    std::fclose(f);
    if (tail.empty()) LOGW("OBB: the game's texts could not be changed for the Switch; they keep saying MOGA");
    return tail;
}

}  // namespace

void start(const std::string& obb_host_path) {
    if (std::getenv("NFSMW_NO_OBB_PATCH")) return;
    hle::filecache::set_tail(obb_host_path, [obb_host_path] { return build_tail(obb_host_path); });
}

}  // namespace game::obb_patch
