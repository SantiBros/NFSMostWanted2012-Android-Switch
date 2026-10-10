#include "hle/jni/jni.h"

#include <mutex>
#include <unordered_set>

#include "core/elf.h"
#include "hle/hle.h"
#include "hle/jni/jni_table.h"

namespace jni {

namespace {

constexpr u32 OBJ_BASE = 0x40000000;
constexpr u32 ID_BASE = 0x30000000;

std::recursive_mutex g_mutex;
std::vector<Object*> g_objects{nullptr};
std::vector<void*> g_ids{nullptr};  // Method* or Field*
std::unordered_map<std::string, Class*> g_classes;
std::unordered_map<std::string, Impl> g_impls;  // "cls|name|sig"
u32 g_env = 0, g_vm = 0;
thread_local jref t_exception = 0;

jref add_object(Object* o) {
    std::lock_guard lk(g_mutex);
    g_objects.push_back(o);
    return OBJ_BASE + (u32)(g_objects.size() - 1) * 4;
}
u32 add_id(void* p) {
    std::lock_guard lk(g_mutex);
    g_ids.push_back(p);
    return ID_BASE + (u32)(g_ids.size() - 1) * 4;
}
void* get_id(u32 id) {
    std::lock_guard lk(g_mutex);
    u32 i = (id - ID_BASE) / 4;
    if (id < ID_BASE || i >= g_ids.size()) return nullptr;
    return g_ids[i];
}
Method* method(u32 id) { return static_cast<Method*>(get_id(id)); }
Field* field(u32 id) { return static_cast<Field*>(get_id(id)); }

std::string norm(std::string n) {
    for (auto& c : n)
        if (c == '.') c = '/';
    return n;
}

void parse_sig(const std::string& sig, std::vector<char>& params, char& ret) {
    size_t i = sig.find('(');
    if (i == std::string::npos) return;
    i++;
    auto one = [&](size_t& k) -> char {
        char c = sig[k];
        if (c == '[') {
            while (sig[k] == '[') k++;
            if (sig[k] == 'L') k = sig.find(';', k);
            k++;
            return 'L';
        }
        if (c == 'L') {
            k = sig.find(';', k) + 1;
            return 'L';
        }
        k++;
        return c;
    };
    while (i < sig.size() && sig[i] != ')') params.push_back(one(i));
    if (i < sig.size()) {
        i++;
        ret = i < sig.size() ? one(i) : 'V';
    }
}

Method* get_method(Class* c, const std::string& name, const std::string& sig) {
    std::lock_guard lk(g_mutex);
    std::string key = name + sig;
    auto it = c->methods.find(key);
    if (it != c->methods.end()) return it->second;
    auto* m = new Method;
    m->cls = c;
    m->name = name;
    m->sig = sig;
    parse_sig(sig, m->params, m->ret);
    auto impl = g_impls.find(c->name + "|" + name + "|" + sig);
    if (impl == g_impls.end()) impl = g_impls.find("*|" + name + "|" + sig);
    if (impl != g_impls.end()) m->impl = impl->second;
    m->id = add_id(m);
    c->methods[key] = m;
    return m;
}

Field* get_field(Class* c, const std::string& name, const std::string& sig, bool is_static) {
    std::lock_guard lk(g_mutex);
    auto it = c->fields.find(name);
    if (it != c->fields.end()) return it->second;
    auto* f = new Field{c, name, sig, is_static, 0};
    f->id = add_id(f);
    c->fields[name] = f;
    return f;
}

Value invoke(Method* m, jref thiz, std::vector<Value>&& args) {
    static const bool trace = std::getenv("NFSMW_TRACE_JNI") != nullptr;
    if (trace) LOGI("JNI call %s.%s%s", m->cls->name.c_str(), m->name.c_str(), m->sig.c_str());
    Call call{thiz, m, std::move(args)};
    // virtual dispatch: prefer an implementation on the receiver's class
    if (Object* o = get(thiz); o && o->cls && o->cls != m->cls && !o->as_class) {
        Method* vm = get_method(o->cls, m->name, m->sig);
        if (vm->impl) return vm->impl(call);
    }
    if (m->impl) return m->impl(call);
    if (!m->warned) {
        m->warned = true;
        LOGD("JNI: unimplemented Java method %s.%s%s", m->cls->name.c_str(), m->name.c_str(), m->sig.c_str());
    }
    return Value{};
}

// ---- argument decoding -------------------------------------------------------
std::vector<Value> args_from_va(Method* m, abi::VarArgs& va) {
    std::vector<Value> out;
    for (char p : m->params) {
        switch (p) {
        case 'J': out.push_back(Value::J((s64)va.next64())); break;
        case 'D': out.push_back(Value::D(va.next_double())); break;
        case 'F': out.push_back(Value::F((float)va.next_double())); break;  // promoted to double
        default: out.push_back(Value{va.next32()}); break;
        }
    }
    return out;
}
std::vector<Value> args_from_jvalues(Method* m, u32 a) {
    std::vector<Value> out;
    for (size_t i = 0; i < m->params.size(); i++, a += 8) {
        switch (m->params[i]) {
        case 'Z': out.push_back(Value::I(mem::r8(a))); break;
        case 'B': out.push_back(Value::I((s8)mem::r8(a))); break;
        case 'C': out.push_back(Value::I(mem::r16(a))); break;
        case 'S': out.push_back(Value::I((s16)mem::r16(a))); break;
        case 'J': case 'D': out.push_back(Value{mem::r64(a)}); break;
        default: out.push_back(Value{mem::r32(a)}); break;
        }
    }
    return out;
}

void set_ret(cpu::GuestThread& t, char type, Value v) {
    auto& r = t.regs();
    switch (type) {
    case 'Z': r[0] = v.raw ? 1 : 0; break;
    case 'B': r[0] = (u32)(s32)(s8)v.raw; break;
    case 'C': r[0] = (u16)v.raw; break;
    case 'S': r[0] = (u32)(s32)(s16)v.raw; break;
    case 'J': case 'D': r[0] = (u32)v.raw; r[1] = (u32)(v.raw >> 32); break;
    case 'V': break;
    default: r[0] = (u32)v.raw; break;
    }
}

// ---- function table implementation ----------------------------------------
enum class Kind { Instance, Nonvirtual, Static };
enum class Mode { Dots, V, A };

thunks::Fn make_call(Kind kind, Mode mode, char ret) {
    return [kind, mode, ret](cpu::GuestThread& t) {
        abi::ArgReader rd(t);
        rd.word();  // env
        jref target = rd.word();
        if (kind == Kind::Nonvirtual) rd.word();  // clazz
        Method* m = method(rd.word());
        if (!m) {
            LOGE("JNI call with invalid method id");
            set_ret(t, ret, Value{});
            return;
        }
        std::vector<Value> args;
        if (mode == Mode::Dots) {
            auto va = rd.get<abi::VarArgs>();
            args = args_from_va(m, va);
        } else if (mode == Mode::V) {
            auto va = abi::VarArgs::from_valist(rd.word());
            args = args_from_va(m, va);
        } else {
            args = args_from_jvalues(m, rd.word());
        }
        Value v = invoke(m, target, std::move(args));
        set_ret(t, ret, v);
    };
}

thunks::Fn make_new_object(Mode mode) {
    return [mode](cpu::GuestThread& t) {
        abi::ArgReader rd(t);
        rd.word();
        Object* co = get(rd.word());
        Method* m = method(rd.word());
        Class* c = co && co->as_class ? co->as_class : find_class("java/lang/Object");
        jref obj = new_object(c);
        if (m) {
            std::vector<Value> args;
            if (mode == Mode::Dots) {
                auto va = rd.get<abi::VarArgs>();
                args = args_from_va(m, va);
            } else if (mode == Mode::V) {
                auto va = abi::VarArgs::from_valist(rd.word());
                args = args_from_va(m, va);
            } else {
                args = args_from_jvalues(m, rd.word());
            }
            if (m->impl) {
                Call call{obj, m, std::move(args)};
                m->impl(call);
            }
        }
        t.regs()[0] = obj;
    };
}

Class* class_of(jref r) {
    Object* o = get(r);
    return o && o->as_class ? o->as_class : nullptr;
}

const char* kTypes[] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
const char kTypeChars[] = {'L', 'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D', 'V'};
const char* kPrimTypes[] = {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
const char kPrimChars[] = {'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D'};

void register_env_functions(std::unordered_map<std::string, thunks::Fn>& fns) {
    using abi::wrap;
    // ---- calls
    for (int i = 0; i < 10; i++) {
        std::string T = kTypes[i];
        char c = kTypeChars[i];
        fns["Call" + T + "Method"] = make_call(Kind::Instance, Mode::Dots, c);
        fns["Call" + T + "MethodV"] = make_call(Kind::Instance, Mode::V, c);
        fns["Call" + T + "MethodA"] = make_call(Kind::Instance, Mode::A, c);
        fns["CallNonvirtual" + T + "Method"] = make_call(Kind::Nonvirtual, Mode::Dots, c);
        fns["CallNonvirtual" + T + "MethodV"] = make_call(Kind::Nonvirtual, Mode::V, c);
        fns["CallNonvirtual" + T + "MethodA"] = make_call(Kind::Nonvirtual, Mode::A, c);
        fns["CallStatic" + T + "Method"] = make_call(Kind::Static, Mode::Dots, c);
        fns["CallStatic" + T + "MethodV"] = make_call(Kind::Static, Mode::V, c);
        fns["CallStatic" + T + "MethodA"] = make_call(Kind::Static, Mode::A, c);
    }
    fns["NewObject"] = make_new_object(Mode::Dots);
    fns["NewObjectV"] = make_new_object(Mode::V);
    fns["NewObjectA"] = make_new_object(Mode::A);

    // ---- fields
    for (int i = 0; i < 9; i++) {
        std::string T = kTypes[i];
        char c = kTypeChars[i];
        fns["Get" + T + "Field"] = [c](cpu::GuestThread& t) {
            auto& r = t.regs();
            Object* o = get(r[1]);
            Field* f = field(r[2]);
            Value v{};
            if (o && f) {
                auto it = o->fields.find(f->name);
                if (it != o->fields.end()) v = it->second;
            }
            set_ret(t, c, v);
        };
        fns["GetStatic" + T + "Field"] = [c](cpu::GuestThread& t) {
            auto& r = t.regs();
            Field* f = field(r[2]);
            Value v{};
            if (f) {
                auto it = f->cls->statics.find(f->name);
                if (it != f->cls->statics.end()) v = it->second;
                else LOGD("JNI: static field %s.%s not set", f->cls->name.c_str(), f->name.c_str());
            }
            set_ret(t, c, v);
        };
        auto setter = [c](cpu::GuestThread& t, bool is_static) {
            abi::ArgReader rd(t);
            rd.word();
            jref target = rd.word();
            Field* f = field(rd.word());
            Value v = (c == 'J' || c == 'D') ? Value{rd.dword()} : Value{rd.word()};
            if (!f) return;
            if (is_static) f->cls->statics[f->name] = v;
            else if (Object* o = get(target)) o->fields[f->name] = v;
        };
        fns["Set" + T + "Field"] = [setter](cpu::GuestThread& t) { setter(t, false); };
        fns["SetStatic" + T + "Field"] = [setter](cpu::GuestThread& t) { setter(t, true); };
    }

    // ---- primitive arrays (storage lives in guest memory, so no copies)
    for (int i = 0; i < 8; i++) {
        std::string T = kPrimTypes[i];
        char c = kPrimChars[i];
        fns["New" + T + "Array"] = [c](cpu::GuestThread& t) { t.regs()[0] = new_array(c, t.regs()[1]); };
        fns["Get" + T + "ArrayElements"] = [](cpu::GuestThread& t) {
            auto& r = t.regs();
            Object* o = get(r[1]);
            if (r[2]) mem::w8(r[2], 0);
            r[0] = o ? o->data : 0;
        };
        fns["Release" + T + "ArrayElements"] = [](cpu::GuestThread&) {};
        fns["Get" + T + "ArrayRegion"] = [c](cpu::GuestThread& t) {
            abi::ArgReader rd(t);
            rd.word();
            Object* o = get(rd.word());
            u32 start = rd.word(), len = rd.word(), buf = rd.word();
            if (o && o->data) std::memmove(mem::host(buf), mem::host(o->data + start * elem_size(c)), len * elem_size(c));
        };
        fns["Set" + T + "ArrayRegion"] = [c](cpu::GuestThread& t) {
            abi::ArgReader rd(t);
            rd.word();
            Object* o = get(rd.word());
            u32 start = rd.word(), len = rd.word(), buf = rd.word();
            if (o && o->data) std::memmove(mem::host(o->data + start * elem_size(c)), mem::host(buf), len * elem_size(c));
        };
    }
    fns["GetPrimitiveArrayCritical"] = fns["GetIntArrayElements"];
    fns["ReleasePrimitiveArrayCritical"] = [](cpu::GuestThread&) {};

    // ---- everything else, with explicit signatures
    auto F = [&fns](const char* name, auto fn) { fns[name] = wrap(fn); };
    F("GetVersion", +[](u32) -> s32 { return 0x00010006; });
    F("DefineClass", +[](u32, u32, u32, u32, u32) -> u32 { return 0; });
    F("FindClass", +[](u32, u32 name) -> u32 { return find_class(mem::str(name))->handle; });
    F("FromReflectedMethod", +[](u32, u32) -> u32 { return 0; });
    F("FromReflectedField", +[](u32, u32) -> u32 { return 0; });
    F("ToReflectedMethod", +[](u32, u32, u32, u32) -> u32 { return 0; });
    F("ToReflectedField", +[](u32, u32, u32, u32) -> u32 { return 0; });
    F("GetSuperclass", +[](u32, u32) -> u32 { return find_class("java/lang/Object")->handle; });
    F("IsAssignableFrom", +[](u32, u32, u32) -> u32 { return 1; });
    F("Throw", +[](u32, u32 obj) -> s32 { t_exception = obj; return 0; });
    F("ThrowNew", +[](u32, u32 cls, u32 msg) -> s32 {
        Class* c = class_of(cls);
        throw_new(c ? c->name : "java/lang/Exception", msg ? mem::str(msg) : "");
        return 0;
    });
    F("ExceptionOccurred", +[](u32) -> u32 { return t_exception; });
    F("ExceptionDescribe", +[](u32) {
        if (Object* o = get(t_exception)) LOGW("JNI exception pending: %s", o->cls->name.c_str());
    });
    F("ExceptionClear", +[](u32) { t_exception = 0; });
    F("ExceptionCheck", +[](u32) -> u32 { return t_exception ? 1 : 0; });
    F("FatalError", +[](u32, u32 msg) { fatal("JNI FatalError: %s", mem::str(msg)); });
    F("PushLocalFrame", +[](u32, s32) -> s32 { return 0; });
    F("PopLocalFrame", +[](u32, u32 res) -> u32 { return res; });
    F("NewGlobalRef", +[](u32, u32 o) -> u32 { return o; });
    F("DeleteGlobalRef", +[](u32, u32) {});
    F("DeleteLocalRef", +[](u32, u32) {});
    F("NewLocalRef", +[](u32, u32 o) -> u32 { return o; });
    F("NewWeakGlobalRef", +[](u32, u32 o) -> u32 { return o; });
    F("DeleteWeakGlobalRef", +[](u32, u32) {});
    F("EnsureLocalCapacity", +[](u32, s32) -> s32 { return 0; });
    F("IsSameObject", +[](u32, u32 a, u32 b) -> u32 { return a == b; });
    F("AllocObject", +[](u32, u32 cls) -> u32 {
        Class* c = class_of(cls);
        return new_object(c ? c : find_class("java/lang/Object"));
    });
    F("GetObjectClass", +[](u32, u32 obj) -> u32 {
        Object* o = get(obj);
        return (o && o->cls ? o->cls : find_class("java/lang/Object"))->handle;
    });
    F("IsInstanceOf", +[](u32, u32 obj, u32 cls) -> u32 {
        Object* o = get(obj);
        Class* c = class_of(cls);
        if (!o) return 1;  // null is an instance of everything per JNI
        if (!c || !o->cls || o->cls == c || c->name == "java/lang/Object") return 1;
        // Exceptions: be precise so the game can tell IOExceptions apart.
        if (o->cls->name.find("Exception") != std::string::npos) {
            if (c->name == "java/lang/Throwable" || c->name == "java/lang/Exception") return 1;
            if (c->name == "java/io/IOException" && o->cls->name.rfind("java/io/", 0) == 0) return 1;
            return 0;
        }
        return 1;
    });
    F("GetMethodID", +[](u32, u32 cls, u32 name, u32 sig) -> u32 {
        Class* c = class_of(cls);
        if (!c) return 0;
        return get_method(c, mem::str(name), mem::str(sig))->id;
    });
    F("GetStaticMethodID", +[](u32, u32 cls, u32 name, u32 sig) -> u32 {
        Class* c = class_of(cls);
        if (!c) return 0;
        return get_method(c, mem::str(name), mem::str(sig))->id;
    });
    F("GetFieldID", +[](u32, u32 cls, u32 name, u32 sig) -> u32 {
        Class* c = class_of(cls);
        return c ? get_field(c, mem::str(name), mem::str(sig), false)->id : 0;
    });
    F("GetStaticFieldID", +[](u32, u32 cls, u32 name, u32 sig) -> u32 {
        Class* c = class_of(cls);
        return c ? get_field(c, mem::str(name), mem::str(sig), true)->id : 0;
    });

    // ---- strings
    F("NewString", +[](u32, u32 chars, s32 len) -> u32 {
        std::string s;
        for (s32 i = 0; i < len; i++) {
            u32 cp = mem::r16(chars + i * 2);
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len) {
                u32 lo = mem::r16(chars + (i + 1) * 2);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
            if (cp < 0x80) s += (char)cp;
            else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
            else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        }
        return new_string(s);
    });
    F("NewStringUTF", +[](u32, u32 s) -> u32 { return s ? new_string(mem::str(s)) : 0; });
    F("GetStringUTFLength", +[](u32, u32 s) -> s32 { return (s32)get_string(s).size(); });
    F("GetStringUTFChars", +[](u32, u32 s, u32 is_copy) -> u32 {
        if (is_copy) mem::w8(is_copy, 1);
        return mem::heap_strdup(get_string(s).c_str());
    });
    F("ReleaseStringUTFChars", +[](u32, u32, u32 chars) { mem::heap_free(chars); });
    F("GetStringLength", +[](u32, u32 s) -> s32 {
        std::string u = get_string(s);
        s32 n = 0;
        for (size_t i = 0; i < u.size(); i++) {
            u8 c = (u8)u[i];
            if ((c & 0xC0) != 0x80) n += (c >= 0xF0) ? 2 : 1;
        }
        return n;
    });
    auto to_utf16 = [](const std::string& u) {
        std::vector<u16> out;
        for (size_t i = 0; i < u.size();) {
            u8 c = (u8)u[i];
            u32 cp;
            int n;
            if (c < 0x80) { cp = c; n = 1; }
            else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
            else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
            else { cp = c & 0x07; n = 4; }
            for (int k = 1; k < n && i + k < u.size(); k++) cp = (cp << 6) | ((u8)u[i + k] & 0x3F);
            i += n;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                out.push_back((u16)(0xD800 + (cp >> 10)));
                out.push_back((u16)(0xDC00 + (cp & 0x3FF)));
            } else {
                out.push_back((u16)cp);
            }
        }
        return out;
    };
    fns["GetStringChars"] = [to_utf16](cpu::GuestThread& t) {
        auto& r = t.regs();
        auto w = to_utf16(get_string(r[1]));
        u32 p = mem::heap_alloc((u32)(w.size() + 1) * 2);
        std::memcpy(mem::host(p), w.data(), w.size() * 2);
        mem::w16(p + (u32)w.size() * 2, 0);
        if (r[2]) mem::w8(r[2], 1);
        r[0] = p;
    };
    fns["GetStringCritical"] = fns["GetStringChars"];
    F("ReleaseStringChars", +[](u32, u32, u32 p) { mem::heap_free(p); });
    F("ReleaseStringCritical", +[](u32, u32, u32 p) { mem::heap_free(p); });
    fns["GetStringRegion"] = [to_utf16](cpu::GuestThread& t) {
        auto& r = t.regs();
        auto w = to_utf16(get_string(r[1]));
        u32 start = r[2], len = r[3], buf = mem::r32(r[13]);
        for (u32 i = 0; i < len && start + i < w.size(); i++) mem::w16(buf + i * 2, w[start + i]);
    };
    fns["GetStringUTFRegion"] = [](cpu::GuestThread& t) {
        auto& r = t.regs();
        std::string s = get_string(r[1]);
        u32 start = r[2], len = r[3], buf = mem::r32(r[13]);
        std::string sub = start < s.size() ? s.substr(start, len) : "";
        std::memcpy(mem::host(buf), sub.c_str(), sub.size() + 1);
    };

    // ---- object arrays
    F("GetArrayLength", +[](u32, u32 a) -> s32 { Object* o = get(a); return o ? (s32)o->length : 0; });
    F("NewObjectArray", +[](u32, s32 len, u32 cls, u32 init) -> u32 {
        (void)cls;
        jref a = new_array('L', (u32)len);
        Object* o = get(a);
        for (s32 i = 0; i < len; i++) mem::w32(o->data + i * 4, init);
        return a;
    });
    F("GetObjectArrayElement", +[](u32, u32 a, s32 i) -> u32 {
        Object* o = get(a);
        return o && (u32)i < o->length ? mem::r32(o->data + i * 4) : 0;
    });
    F("SetObjectArrayElement", +[](u32, u32 a, s32 i, u32 v) {
        Object* o = get(a);
        if (o && (u32)i < o->length) mem::w32(o->data + i * 4, v);
    });

    // ---- natives, monitors, VM, buffers
    F("RegisterNatives", +[](u32, u32 cls, u32 methods, s32 n) -> s32 {
        Class* c = class_of(cls);
        if (!c) return -1;
        for (s32 i = 0; i < n; i++) {
            u32 e = methods + i * 12;
            std::string name = mem::str(mem::r32(e)), sig = mem::str(mem::r32(e + 4));
            c->natives[name + sig] = mem::r32(e + 8);
            c->natives[name] = mem::r32(e + 8);
            LOGD("JNI: RegisterNatives %s.%s%s", c->name.c_str(), name.c_str(), sig.c_str());
        }
        return 0;
    });
    F("UnregisterNatives", +[](u32, u32) -> s32 { return 0; });
    F("MonitorEnter", +[](u32, u32) -> s32 { return 0; });
    F("MonitorExit", +[](u32, u32) -> s32 { return 0; });
    F("GetJavaVM", +[](u32, u32 out) -> s32 { mem::w32(out, g_vm); return 0; });
    F("NewDirectByteBuffer", +[](u32, u32 addr, s64 cap) -> u32 {
        jref b = new_object("java/nio/ByteBuffer");
        get(b)->data = addr;
        get(b)->length = (u32)cap;
        return b;
    });
    F("GetDirectBufferAddress", +[](u32, u32 b) -> u32 { Object* o = get(b); return o ? o->data : 0; });
    F("GetDirectBufferCapacity", +[](u32, u32 b) -> s64 { Object* o = get(b); return o ? o->length : -1; });
    F("GetObjectRefType", +[](u32, u32) -> s32 { return 1; });
}

}  // namespace

std::string Call::s(size_t k) const { return get_string(l(k)); }

u32 env() { return g_env; }
u32 vm() { return g_vm; }

u32 elem_size(char e) {
    switch (e) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'J': case 'D': return 8;
    default: return 4;
    }
}

Class* find_class(const std::string& raw) {
    std::string name = norm(raw);
    std::lock_guard lk(g_mutex);
    auto it = g_classes.find(name);
    if (it != g_classes.end()) return it->second;
    auto* c = new Class;
    c->name = name;
    g_classes[name] = c;
    auto* co = new Object;
    co->as_class = c;
    c->handle = add_object(co);
    co->cls = name == "java/lang/Class" ? c : find_class("java/lang/Class");
    return c;
}

Object* get(jref r) {
    std::lock_guard lk(g_mutex);
    u32 i = (r - OBJ_BASE) / 4;
    if (r < OBJ_BASE || i >= g_objects.size()) return nullptr;
    return g_objects[i];
}

jref new_object(Class* c) {
    auto* o = new Object;
    o->cls = c;
    return add_object(o);
}
jref new_object(const std::string& cls) { return new_object(find_class(cls)); }

jref new_string(const std::string& s) {
    auto* o = new Object;
    o->cls = find_class("java/lang/String");
    o->is_string = true;
    o->str = s;
    return add_object(o);
}

std::string get_string(jref r) {
    Object* o = get(r);
    return o && o->is_string ? o->str : std::string();
}

jref new_array(char e, u32 len) {
    auto* o = new Object;
    o->cls = find_class(std::string("[") + e);
    o->elem = e;
    o->length = len;
    o->data = mem::heap_calloc(len ? len : 1, elem_size(e));
    return add_object(o);
}

jref new_object_array(const std::string& elem_cls, const std::vector<jref>& items) {
    (void)elem_cls;
    jref a = new_array('L', (u32)items.size());
    Object* o = get(a);
    for (size_t i = 0; i < items.size(); i++) mem::w32(o->data + (u32)i * 4, items[i]);
    return a;
}

void def(const std::string& cls, const std::string& name, const std::string& sig, Impl impl) {
    std::lock_guard lk(g_mutex);
    std::string c = norm(cls);
    g_impls[c + "|" + name + "|" + sig] = impl;
    if (c != "*") get_method(find_class(c), name, sig)->impl = impl;
}

void set_static(const std::string& cls, const std::string& f, Value v) { find_class(cls)->statics[f] = v; }
void set_field(jref obj, const std::string& f, Value v) {
    if (Object* o = get(obj)) o->fields[f] = v;
}

void throw_new(const std::string& cls, const std::string& msg) {
    jref e = new_object(cls);
    set_field(e, "message", Value::L(new_string(msg)));
    t_exception = e;
    LOGD("JNI: throwing %s(%s)", cls.c_str(), msg.c_str());
}

static std::string mangle(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '/' || c == '.') out += '_';
        else if (c == '_') out += "_1";
        else if (c == ';') out += "_2";
        else if (c == '[') out += "_3";
        else out += c;
    }
    return out;
}

static u32 native_addr(const std::string& cls, const std::string& name, const std::string& sig) {
    Class* c = find_class(cls);
    auto it = c->natives.find(name + sig);
    if (it == c->natives.end()) it = c->natives.find(name);
    if (it != c->natives.end()) return it->second;
    return elf::lookup_global("Java_" + mangle(norm(cls)) + "_" + mangle(name));
}

bool has_native(const std::string& cls, const std::string& name) { return native_addr(cls, name, "") != 0; }

Value call_native(const std::string& cls, const std::string& name, const std::string& sig, jref thiz,
                  const std::vector<Value>& args) {
    u32 fn = native_addr(cls, name, sig);
    if (!fn) {
        LOGW("JNI: native %s.%s not found", cls.c_str(), name.c_str());
        return Value{};
    }
    std::vector<char> params;
    char ret = 'V';
    parse_sig(sig, params, ret);
    std::vector<u32> words{g_env, thiz ? thiz : find_class(cls)->handle};
    for (size_t i = 0; i < args.size(); i++) {
        char p = i < params.size() ? params[i] : 'I';
        if (p == 'J' || p == 'D') {
            if (words.size() & 1) words.push_back(0);
            words.push_back((u32)args[i].raw);
            words.push_back((u32)(args[i].raw >> 32));
        } else {
            words.push_back((u32)args[i].raw);
        }
    }
    u64 r = cpu::current().call(fn, words.data(), words.size());
    if (ret == 'J' || ret == 'D') return Value{r};
    return Value{(u64)(u32)r};
}

void init() {
    std::unordered_map<std::string, thunks::Fn> fns;
    register_env_functions(fns);
    // The function table lives in guest memory; each slot points at a host thunk.
    u32 table = mem::static_alloc(kNumEnvFunctions * 4 + 64, 8);
    int implemented = 0;
    for (int i = 0; i < kNumEnvFunctions; i++) {
        std::string n = kEnvFunctionNames[i];
        if (n.rfind("reserved", 0) == 0) continue;
        auto it = fns.find(n);
        u32 a;
        if (it != fns.end()) {
            // Anything that may run Java code (and thus guest natives) is re-entrant.
            bool re = n.rfind("Call", 0) == 0 || n.rfind("NewObject", 0) == 0;
            a = thunks::add("JNI::" + n, it->second, re);
            implemented++;
        } else {
            a = thunks::make_missing("JNI::" + n);
        }
        mem::w32(table + i * 4, a);
    }
    g_env = mem::static_alloc(8, 8);
    mem::w32(g_env, table);

    u32 itable = mem::static_alloc(8 * 4, 8);
    auto vmf = [&](int idx, const char* name, auto fn) { mem::w32(itable + idx * 4, thunks::add(std::string("JNI::VM::") + name, abi::wrap(fn))); };
    vmf(3, "DestroyJavaVM", +[](u32) -> s32 { return 0; });
    vmf(4, "AttachCurrentThread", +[](u32, u32 penv, u32) -> s32 { mem::w32(penv, g_env); return 0; });
    vmf(5, "DetachCurrentThread", +[](u32) -> s32 { return 0; });
    vmf(6, "GetEnv", +[](u32, u32 penv, s32) -> s32 { mem::w32(penv, g_env); return 0; });
    vmf(7, "AttachCurrentThreadAsDaemon", +[](u32, u32 penv, u32) -> s32 { mem::w32(penv, g_env); return 0; });
    g_vm = mem::static_alloc(8, 8);
    mem::w32(g_vm, itable);
    LOGI("JNI: %d/%d environment functions implemented", implemented, kNumEnvFunctions - 4);
    register_java_classes();
}

}  // namespace jni
