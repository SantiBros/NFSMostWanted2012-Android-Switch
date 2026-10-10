// A small fake Java world for the game's JNI calls.
//
// The game's native code talks to Java through the JNIEnv function table.
// There is no Java VM here: classes, methods and fields are created on demand
// when the game asks for them, and calls are routed to C++ implementations
// registered with jni::def(). Anything without an implementation logs once and
// returns 0/null, which is what keeps optional Android services (billing,
// Facebook, telemetry...) from blocking the game.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common.h"

namespace jni {

using jref = u32;

struct Value {
    u64 raw = 0;
    static Value I(s32 v) { return Value{(u64)(u32)v}; }
    static Value J(s64 v) { return Value{(u64)v}; }
    static Value Z(bool v) { return Value{v ? 1u : 0u}; }
    static Value F(float v) { return Value{bit_cast_<u32>(v)}; }
    static Value D(double v) { return Value{bit_cast_<u64>(v)}; }
    static Value L(jref v) { return Value{v}; }
    s32 i() const { return (s32)(u32)raw; }
    s64 j() const { return (s64)raw; }
    float f() const { return bit_cast_<float>((u32)raw); }
    double d() const { return bit_cast_<double>(raw); }
    jref l() const { return (u32)raw; }
};

struct Class;
struct Method;

struct Object {
    Class* cls = nullptr;
    bool is_string = false;
    std::string str;                          // java.lang.String payload (UTF-8)
    char elem = 0;                            // array element type ('I', 'B', 'L'...), 0 if not array
    u32 length = 0;                           // array length
    u32 data = 0;                             // array storage in guest memory
    std::unordered_map<std::string, Value> fields;
    std::shared_ptr<void> native;             // host payload for special classes
    Class* as_class = nullptr;                // set for java.lang.Class objects
};

struct Call {
    jref thiz = 0;         // receiver (instance) or class handle (static)
    Method* method = nullptr;
    std::vector<Value> args;
    s32 i(size_t k) const { return args.at(k).i(); }
    s64 j(size_t k) const { return args.at(k).j(); }
    float f(size_t k) const { return args.at(k).f(); }
    double d(size_t k) const { return args.at(k).d(); }
    bool z(size_t k) const { return args.at(k).i() != 0; }
    jref l(size_t k) const { return args.at(k).l(); }
    std::string s(size_t k) const;  // String argument as UTF-8
};

using Impl = std::function<Value(Call&)>;

struct Method {
    Class* cls = nullptr;
    std::string name, sig;
    std::vector<char> params;  // one type char per parameter ('L' for objects/arrays)
    char ret = 'V';
    Impl impl;
    bool warned = false;
    u32 id = 0;
};

struct Field {
    Class* cls = nullptr;
    std::string name, sig;
    bool is_static = false;
    u32 id = 0;
};

struct Class {
    std::string name;  // "java/lang/String"
    jref handle = 0;
    std::unordered_map<std::string, Method*> methods;  // name + sig
    std::unordered_map<std::string, Field*> fields;
    std::unordered_map<std::string, Value> statics;
    std::unordered_map<std::string, u32> natives;  // RegisterNatives: name + sig -> guest fn
};

void init();
u32 env();  // guest JNIEnv*
u32 vm();   // guest JavaVM*

Class* find_class(const std::string& name);
Object* get(jref r);
jref new_object(const std::string& cls);
jref new_object(Class* cls);
jref new_string(const std::string& utf8);
std::string get_string(jref r);
jref new_array(char elem, u32 length);
jref new_object_array(const std::string& elem_cls, const std::vector<jref>& items);
u32 elem_size(char elem);

// Register the implementation of a Java method (static or instance).
void def(const std::string& cls, const std::string& name, const std::string& sig, Impl impl);
void set_static(const std::string& cls, const std::string& field, Value v);
void set_field(jref obj, const std::string& field, Value v);

// Java exceptions: raised by implementations, observed by ExceptionCheck().
void throw_new(const std::string& cls, const std::string& msg);

// Call a native method of the game: an exported Java_* symbol or one
// registered through RegisterNatives. Arguments follow the method signature.
bool has_native(const std::string& cls, const std::string& name);
Value call_native(const std::string& cls, const std::string& name, const std::string& sig, jref thiz,
                  const std::vector<Value>& args);

// Implemented in java_classes.cpp
void register_java_classes();

}  // namespace jni
