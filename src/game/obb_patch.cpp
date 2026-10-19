#include "game/obb_patch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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

}  // namespace

std::string patch_string_table(const std::string& sbin, int* changed) {
    if (changed) *changed = 0;
    if (sbin.size() < 8 || sbin.compare(0, 4, "SBIN") != 0) return sbin;
    struct Chunk {
        std::string tag, data;
    };
    std::vector<Chunk> chunks;
    for (size_t p = 8; p + 12 <= sbin.size();) {
        const u32 size = rd32(sbin, p + 4);
        if (p + 12 + size > sbin.size()) return sbin;
        chunks.push_back({sbin.substr(p, 4), sbin.substr(p + 12, size)});
        p += 12 + size;
    }
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
    // then 0xFFFFFFFF.
    std::unordered_map<u32, std::string> key_of;  // text's string id -> key
    for (size_t p = 0; p + 8 <= data->data.size(); p += 4) {
        if (rd32(data->data, p + 4) != 0xFFFFFFFFu) continue;
        const u16 k = rd16(data->data, p), v = rd16(data->data, p + 2);
        if (k < n && v < n && is_key(strs[k]) && !key_of.count(v)) key_of[v] = strs[k];
    }
    int count = 0;
    for (auto& [v, key] : key_of) {
        const std::string text = new_text(key, strs[v]);
        if (text == strs[v]) continue;
        // The new text goes to the end of the string data; the old one stays
        // where it is, unused.
        wr32(chdr->data, v * 8, (u32)cdat->data.size());
        wr32(chdr->data, v * 8 + 4, (u32)text.size());
        cdat->data += text;
        cdat->data.push_back('\0');
        count++;
    }
    if (changed) *changed = count;
    if (!count) return sbin;
    std::string out = sbin.substr(0, 8);
    for (const Chunk& c : chunks) {
        out += c.tag;
        put32(out, (u32)c.data.size());
        put32(out, fnv1(c.data));
        out += c.data;
    }
    return out;
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
        int files = 0, texts = 0;
        for (size_t p = 0; p + 46 <= cd.size() && rd32(cd, p) == 0x02014B50u;) {
            const u16 method = rd16(cd, p + 10), nlen = rd16(cd, p + 28), xlen = rd16(cd, p + 30), clen = rd16(cd, p + 32);
            const u32 csize = rd32(cd, p + 20), usize = rd32(cd, p + 24), local = rd32(cd, p + 42);
            const std::string name = cd.substr(p + 46, nlen);
            const size_t next = p + 46 + nlen + xlen + clen;
            if (name.rfind("published/stringdata/", 0) == 0 && name.find("/nfsmw_android.sb") != std::string::npos &&
                (method == 0 || method == 8) && usize < (16u << 20)) {
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
                    const std::string patched = plain.empty() ? plain : patch_string_table(plain, &n);
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
                        texts += n;
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
        LOGI("OBB: %d string tables with %d texts changed for the Switch (MOGA -> Switch), %zu KiB added in memory, "
             "%.0f ms",
             files, texts, tail.size() / 1024,
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
