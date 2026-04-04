// F++ Standard Library
// All stdlib modules registered as native functions into the VM

#ifdef _WIN32
#  ifndef _USE_MATH_DEFINES
#    define _USE_MATH_DEFINES
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define _CRT_SECURE_NO_WARNINGS
#endif
#include "../include/runtime.hpp"
#include <algorithm>
#include <numeric>
#include <random>
#include <chrono>
#include <filesystem>
#ifdef _WIN32
#  include <direct.h>
#else
#  include <unistd.h>
#endif
#include <fstream>
#include <sstream>
#include <regex>
#include <cmath>
#include <climits>
#include <complex>
#include <queue>
#include <stack>
#include <set>
#include <map>
#include <bitset>
#include <charconv>
#include <optional>

namespace fpp {
namespace stdlib {

using VR = runtime::ValueRef;
using VM = runtime::VM;
using V  = runtime::Value;

static auto& arr(VR& v) { return *v->arrayData; }
static VR nil()          { return V::makeNil(); }
static VR ok(VR v)       { return V::makeOk(std::move(v)); }
static VR err(std::string m) { return V::makeErr(V::makeString(std::move(m))); }
static VR boolean(bool b)   { return V::makeBool(b); }
static int64_t toI(const VR& v) {
    switch (v->kind) {
    case runtime::ValueKind::I8:  return v->prim.i8;
    case runtime::ValueKind::I16: return v->prim.i16;
    case runtime::ValueKind::I32: return v->prim.i32;
    case runtime::ValueKind::I64: return v->prim.i64;
    case runtime::ValueKind::U8:  return v->prim.u8;
    case runtime::ValueKind::U16: return v->prim.u16;
    case runtime::ValueKind::U32: return v->prim.u32;
    case runtime::ValueKind::U64: return (int64_t)v->prim.u64;
    case runtime::ValueKind::F32: return (int64_t)v->prim.f32;
    case runtime::ValueKind::F64: return (int64_t)v->prim.f64;
    default: return 0;
    }
}
static double toD(const VR& v) {
    if (v->kind == runtime::ValueKind::F64) return v->prim.f64;
    if (v->kind == runtime::ValueKind::F32) return v->prim.f32;
    return (double)toI(v);
}

// ─── std::algo ────────────────────────────────────────────────────────────────
static std::unordered_map<std::string, VR> makeAlgoModule() {
    std::unordered_map<std::string, VR> m;

    m["map"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeArray({});
        std::vector<VR> result;
        for (auto& elem : *args[0]->arrayData) {
            if (args[1]->kind == runtime::ValueKind::NativeFunc)
                result.push_back(args[1]->nativeFn({elem}, vm));
            else if (args[1]->closureData)
                result.push_back(args[1]->closureData->fn({elem}, vm));
            else result.push_back(elem);
        }
        return V::makeArray(std::move(result));
    });

    m["filter"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeArray({});
        std::vector<VR> result;
        for (auto& elem : *args[0]->arrayData) {
            VR keep = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({elem}, vm)
                : (args[1]->closureData ? args[1]->closureData->fn({elem}, vm) : V::makeBool(false));
            if (keep->isTruthy()) result.push_back(elem);
        }
        return V::makeArray(std::move(result));
    });

    m["reduce"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 3 || !args[0]->arrayData) return args.size() > 2 ? args[2] : V::makeNil();
        VR acc = args[2]; // initial value
        for (auto& elem : *args[0]->arrayData) {
            if (args[1]->kind == runtime::ValueKind::NativeFunc)
                acc = args[1]->nativeFn({acc, elem}, vm);
            else if (args[1]->closureData)
                acc = args[1]->closureData->fn({acc, elem}, vm);
        }
        return acc;
    });

    m["flat_map"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeArray({});
        std::vector<VR> result;
        for (auto& elem : *args[0]->arrayData) {
            VR mapped = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({elem}, vm) : elem;
            if (mapped->arrayData)
                for (auto& e : *mapped->arrayData) result.push_back(e);
            else result.push_back(mapped);
        }
        return V::makeArray(std::move(result));
    });

    m["zip"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->arrayData || !args[1]->arrayData) return V::makeArray({});
        size_t n = std::min(args[0]->arrayData->size(), args[1]->arrayData->size());
        std::vector<VR> result;
        for (size_t i = 0; i < n; ++i)
            result.push_back(V::makeArray({(*args[0]->arrayData)[i], (*args[1]->arrayData)[i]}));
        return V::makeArray(std::move(result));
    });

    m["enumerate"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->arrayData) return V::makeArray({});
        std::vector<VR> result;
        for (size_t i = 0; i < args[0]->arrayData->size(); ++i)
            result.push_back(V::makeArray({V::makeI64(i), (*args[0]->arrayData)[i]}));
        return V::makeArray(std::move(result));
    });

    m["find"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeNone();
        for (auto& elem : *args[0]->arrayData) {
            VR match = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({elem}, vm) : V::makeBool(false);
            if (match->isTruthy()) return V::makeSome(elem);
        }
        return V::makeNone();
    });

    m["find_index"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeNone();
        for (size_t i = 0; i < args[0]->arrayData->size(); ++i) {
            auto& elem = (*args[0]->arrayData)[i];
            VR match = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({elem}, vm) : V::makeBool(false);
            if (match->isTruthy()) return V::makeSome(V::makeI64(i));
        }
        return V::makeNone();
    });

    m["any"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeBool(false);
        for (auto& elem : *args[0]->arrayData) {
            VR r = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({elem}, vm) : V::makeBool(false);
            if (r->isTruthy()) return V::makeBool(true);
        }
        return V::makeBool(false);
    });

    m["all"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeBool(true);
        for (auto& elem : *args[0]->arrayData) {
            VR r = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({elem}, vm) : V::makeBool(false);
            if (!r->isTruthy()) return V::makeBool(false);
        }
        return V::makeBool(true);
    });

    m["none_"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeBool(true);
        for (auto& elem : *args[0]->arrayData) {
            VR r = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({elem}, vm) : V::makeBool(false);
            if (r->isTruthy()) return V::makeBool(false);
        }
        return V::makeBool(true);
    });

    m["sort_by"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return nil();
        std::stable_sort(args[0]->arrayData->begin(), args[0]->arrayData->end(),
            [&](const VR& a, const VR& b) -> bool {
                VR r = (args[1]->kind == runtime::ValueKind::NativeFunc)
                    ? args[1]->nativeFn({a, b}, vm) : V::makeBool(false);
                return r->isTruthy();
            });
        return nil();
    });

    m["group_by"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeMap({});
        std::unordered_map<std::string, VR> groups;
        for (auto& elem : *args[0]->arrayData) {
            VR key = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({elem}, vm) : elem;
            std::string k = key->toString();
            if (!groups.count(k)) groups[k] = V::makeArray({});
            groups[k]->arrayData->push_back(elem);
        }
        return V::makeMap(std::move(groups));
    });

    m["flatten"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->arrayData) return V::makeArray({});
        std::vector<VR> result;
        for (auto& elem : *args[0]->arrayData) {
            if (elem->arrayData) for (auto& e : *elem->arrayData) result.push_back(e);
            else result.push_back(elem);
        }
        return V::makeArray(std::move(result));
    });

    m["unique"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->arrayData) return V::makeArray({});
        std::vector<VR> result;
        std::set<std::string> seen;
        for (auto& elem : *args[0]->arrayData) {
            std::string k = elem->toString();
            if (!seen.count(k)) { seen.insert(k); result.push_back(elem); }
        }
        return V::makeArray(std::move(result));
    });

    m["take"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeArray({});
        size_t n = std::min((size_t)toI(args[1]), args[0]->arrayData->size());
        return V::makeArray(std::vector<VR>(args[0]->arrayData->begin(), args[0]->arrayData->begin() + n));
    });

    m["skip"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeArray({});
        size_t n = std::min((size_t)toI(args[1]), args[0]->arrayData->size());
        return V::makeArray(std::vector<VR>(args[0]->arrayData->begin() + n, args[0]->arrayData->end()));
    });

    m["sum"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->arrayData) return V::makeI64(0);
        double total = 0;
        for (auto& e : *args[0]->arrayData) total += toD(e);
        return V::makeF64(total);
    });

    m["product"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->arrayData) return V::makeI64(1);
        double total = 1;
        for (auto& e : *args[0]->arrayData) total *= toD(e);
        return V::makeF64(total);
    });

    m["min_by"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData || args[0]->arrayData->empty()) return V::makeNone();
        VR best = args[0]->arrayData->front();
        double bestScore = toD((args[1]->kind == runtime::ValueKind::NativeFunc)
            ? args[1]->nativeFn({best}, vm) : best);
        for (size_t i = 1; i < args[0]->arrayData->size(); ++i) {
            auto& e = (*args[0]->arrayData)[i];
            double score = toD((args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({e}, vm) : e);
            if (score < bestScore) { best = e; bestScore = score; }
        }
        return V::makeSome(best);
    });

    m["max_by"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData || args[0]->arrayData->empty()) return V::makeNone();
        VR best = args[0]->arrayData->front();
        double bestScore = toD((args[1]->kind == runtime::ValueKind::NativeFunc)
            ? args[1]->nativeFn({best}, vm) : best);
        for (size_t i = 1; i < args[0]->arrayData->size(); ++i) {
            auto& e = (*args[0]->arrayData)[i];
            double score = toD((args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({e}, vm) : e);
            if (score > bestScore) { best = e; bestScore = score; }
        }
        return V::makeSome(best);
    });

    m["chunk"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeArray({});
        size_t n = std::max((size_t)1, (size_t)toI(args[1]));
        std::vector<VR> chunks;
        auto& data = *args[0]->arrayData;
        for (size_t i = 0; i < data.size(); i += n) {
            std::vector<VR> chunk(data.begin()+i, data.begin()+std::min(i+n, data.size()));
            chunks.push_back(V::makeArray(std::move(chunk)));
        }
        return V::makeArray(std::move(chunks));
    });

    m["window"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeArray({});
        size_t n = (size_t)toI(args[1]);
        auto& data = *args[0]->arrayData;
        std::vector<VR> windows;
        if (data.size() < n) return V::makeArray({});
        for (size_t i = 0; i <= data.size()-n; ++i)
            windows.push_back(V::makeArray(std::vector<VR>(data.begin()+i, data.begin()+i+n)));
        return V::makeArray(std::move(windows));
    });

    m["partition"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeArray({V::makeArray({}), V::makeArray({})});
        std::vector<VR> trueVec, falseVec;
        for (auto& e : *args[0]->arrayData) {
            VR r = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({e}, vm) : V::makeBool(false);
            (r->isTruthy() ? trueVec : falseVec).push_back(e);
        }
        return V::makeArray({V::makeArray(std::move(trueVec)), V::makeArray(std::move(falseVec))});
    });

    m["count_if"] = V::makeNative([](std::vector<VR> args, VM& vm) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeI64(0);
        int64_t count = 0;
        for (auto& e : *args[0]->arrayData) {
            VR r = (args[1]->kind == runtime::ValueKind::NativeFunc)
                ? args[1]->nativeFn({e}, vm) : V::makeBool(false);
            if (r->isTruthy()) ++count;
        }
        return V::makeI64(count);
    });

    m["range"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        int64_t start = args.size() > 1 ? toI(args[0]) : 0;
        int64_t end   = args.size() > 1 ? toI(args[1]) : toI(args[0]);
        int64_t step  = args.size() > 2 ? toI(args[2]) : 1;
        if (step == 0) return V::makeArray({});
        std::vector<VR> result;
        for (int64_t i = start; step > 0 ? i < end : i > end; i += step)
            result.push_back(V::makeI64(i));
        return V::makeArray(std::move(result));
    });

    m["binary_search"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->arrayData) return V::makeNone();
        auto& data = *args[0]->arrayData;
        int64_t lo = 0, hi = (int64_t)data.size() - 1;
        while (lo <= hi) {
            int64_t mid = lo + (hi - lo) / 2;
            int cmp = data[mid]->compare(*args[1]);
            if (cmp == 0) return V::makeSome(V::makeI64(mid));
            if (cmp < 0) lo = mid + 1; else hi = mid - 1;
        }
        return V::makeNone();
    });

    m["quicksort"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->arrayData) return nil();
        std::function<void(std::vector<VR>&, int, int)> qs = [&](std::vector<VR>& a, int lo, int hi) {
            if (lo >= hi) return;
            VR pivot = a[hi];
            int i = lo - 1;
            for (int j = lo; j < hi; ++j)
                if (a[j]->compare(*pivot) <= 0) std::swap(a[++i], a[j]);
            std::swap(a[i+1], a[hi]);
            int p = i + 1;
            qs(a, lo, p - 1);
            qs(a, p + 1, hi);
        };
        qs(*args[0]->arrayData, 0, (int)args[0]->arrayData->size()-1);
        return nil();
    });

    m["merge_sort"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->arrayData) return nil();
        std::function<void(std::vector<VR>&)> ms = [&](std::vector<VR>& a) {
            if (a.size() <= 1) return;
            size_t mid = a.size() / 2;
            std::vector<VR> left(a.begin(), a.begin()+mid), right(a.begin()+mid, a.end());
            ms(left); ms(right);
            size_t i = 0, j = 0, k = 0;
            while (i < left.size() && j < right.size())
                a[k++] = left[i]->compare(*right[j]) <= 0 ? left[i++] : right[j++];
            while (i < left.size()) a[k++] = left[i++];
            while (j < right.size()) a[k++] = right[j++];
        };
        ms(*args[0]->arrayData);
        return nil();
    });

    return m;
}

// ─── std::math ───────────────────────────────────────────────────────────────
static std::unordered_map<std::string, VR> makeMathModule() {
    std::unordered_map<std::string, VR> m;
    m["PI"]  = V::makeF64(M_PI);
    m["E"]   = V::makeF64(M_E);
    m["TAU"] = V::makeF64(2 * M_PI);
    m["INF"] = V::makeF64(std::numeric_limits<double>::infinity());
    m["NAN"] = V::makeF64(std::numeric_limits<double>::quiet_NaN());
    m["I64_MAX"] = V::makeI64(INT64_MAX);
    m["I64_MIN"] = V::makeI64(INT64_MIN);
    m["F64_MAX"] = V::makeF64(std::numeric_limits<double>::max());
    m["F64_EPSILON"] = V::makeF64(std::numeric_limits<double>::epsilon());

    // FIX: Use typed function pointers with explicit double overload selection
    // to avoid MSVC overload resolution failure on functions like atan2.
    // For functions with unique double overloads, cast is safe.
    // For ambiguous ones (atan2, pow, hypot, fmod, ldexp), use lambdas.
    using MathFn1 = double(*)(double);
    auto mathFn1 = [](MathFn1 fn) -> VR {
        return V::makeNative([fn](std::vector<VR> a, VM&) -> VR {
            return V::makeF64(fn(toD(a[0])));
        });
    };

    m["sin"]    = mathFn1(static_cast<MathFn1>(std::sin));
    m["cos"]    = mathFn1(static_cast<MathFn1>(std::cos));
    m["tan"]    = mathFn1(static_cast<MathFn1>(std::tan));
    m["asin"]   = mathFn1(static_cast<MathFn1>(std::asin));
    m["acos"]   = mathFn1(static_cast<MathFn1>(std::acos));
    m["atan"]   = mathFn1(static_cast<MathFn1>(std::atan));
    m["sinh"]   = mathFn1(static_cast<MathFn1>(std::sinh));
    m["cosh"]   = mathFn1(static_cast<MathFn1>(std::cosh));
    m["tanh"]   = mathFn1(static_cast<MathFn1>(std::tanh));
    m["exp"]    = mathFn1(static_cast<MathFn1>(std::exp));
    m["exp2"]   = mathFn1(static_cast<MathFn1>(std::exp2));
    m["log"]    = mathFn1(static_cast<MathFn1>(std::log));
    m["log2"]   = mathFn1(static_cast<MathFn1>(std::log2));
    m["log10"]  = mathFn1(static_cast<MathFn1>(std::log10));
    m["sqrt"]   = mathFn1(static_cast<MathFn1>(std::sqrt));
    m["cbrt"]   = mathFn1(static_cast<MathFn1>(std::cbrt));
    m["abs"]    = mathFn1(static_cast<MathFn1>(std::fabs));
    m["ceil"]   = mathFn1(static_cast<MathFn1>(std::ceil));
    m["floor"]  = mathFn1(static_cast<MathFn1>(std::floor));
    m["round"]  = mathFn1(static_cast<MathFn1>(std::round));
    m["trunc"]  = mathFn1(static_cast<MathFn1>(std::trunc));
    m["erf"]    = mathFn1(static_cast<MathFn1>(std::erf));
    m["erfc"]   = mathFn1(static_cast<MathFn1>(std::erfc));
    m["lgamma"] = mathFn1(static_cast<MathFn1>(std::lgamma));
    m["tgamma"] = mathFn1(static_cast<MathFn1>(std::tgamma));

    // FIX: Use lambdas for 2-arg math functions to avoid overload ambiguity on MSVC
    m["atan2"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        return V::makeF64(std::atan2(toD(a[0]), toD(a[1])));
    });
    m["pow"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        return V::makeF64(std::pow(toD(a[0]), toD(a[1])));
    });
    m["fmod"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        return V::makeF64(std::fmod(toD(a[0]), toD(a[1])));
    });
    m["hypot"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        return V::makeF64(std::hypot(toD(a[0]), toD(a[1])));
    });
    m["ldexp"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        return V::makeF64(std::ldexp(toD(a[0]), (int)toI(a[1])));
    });

#if !defined(_WIN32)
    m["j0"] = mathFn1(static_cast<MathFn1>(::j0));
    m["j1"] = mathFn1(static_cast<MathFn1>(::j1));
#else
    m["j0"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        return V::makeF64(std::cyl_bessel_j(0.0, toD(a[0])));
    });
    m["j1"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        return V::makeF64(std::cyl_bessel_j(1.0, toD(a[0])));
    });
#endif

    m["clamp"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        double v = toD(a[0]), lo = toD(a[1]), hi = toD(a[2]);
        return V::makeF64(std::clamp(v, lo, hi));
    });
    m["lerp"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        double lo = toD(a[0]), hi = toD(a[1]), t = toD(a[2]);
        return V::makeF64(lo + t * (hi - lo));
    });
    m["gcd"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t x = toI(a[0]), y = toI(a[1]);
        while (y) { int64_t t = y; y = x % y; x = t; }
        return V::makeI64(x < 0 ? -x : x);
    });
    m["lcm"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t x = toI(a[0]), y = toI(a[1]);
        if (!x || !y) return V::makeI64(0);
        int64_t g = x; int64_t tmp = y;
        while (tmp) { int64_t t = tmp; tmp = g % tmp; g = t; }
        return V::makeI64(std::abs(x / g * y));
    });
    m["is_nan"]  = V::makeNative([](std::vector<VR> a, VM&) -> VR { return V::makeBool(std::isnan(toD(a[0]))); });
    m["is_inf"]  = V::makeNative([](std::vector<VR> a, VM&) -> VR { return V::makeBool(std::isinf(toD(a[0]))); });
    m["is_prime"]= V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t n = toI(a[0]);
        if (n < 2) return V::makeBool(false);
        if (n == 2) return V::makeBool(true);
        if (n % 2 == 0) return V::makeBool(false);
        for (int64_t i = 3; i * i <= n; i += 2)
            if (n % i == 0) return V::makeBool(false);
        return V::makeBool(true);
    });
    m["next_prime"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t n = toI(a[0]) + 1;
        while (true) {
            bool prime = n >= 2;
            if (prime && n > 2 && n % 2 == 0) prime = false;
            if (prime) for (int64_t i = 3; prime && i*i <= n; i+=2) if (n%i==0) prime=false;
            if (prime) return V::makeI64(n);
            ++n;
        }
    });
    m["factorial"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t n = toI(a[0]);
        if (n < 0) return V::makeErr(V::makeString("factorial: negative input"));
        int64_t result = 1;
        for (int64_t i = 2; i <= n; ++i) result *= i;
        return ok(V::makeI64(result));
    });
    m["fibonacci"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t n = toI(a[0]);
        if (n < 0) return V::makeI64(0);
        if (n <= 1) return V::makeI64(n);
        int64_t a2=0, b=1;
        for (int64_t i=2; i<=n; ++i) { int64_t c=a2+b; a2=b; b=c; }
        return V::makeI64(b);
    });
    m["binomial"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t n = toI(a[0]), k = toI(a[1]);
        if (k < 0 || k > n) return V::makeI64(0);
        if (k == 0 || k == n) return V::makeI64(1);
        if (k > n - k) k = n - k;
        int64_t result = 1;
        for (int64_t i = 0; i < k; ++i) { result *= (n - i); result /= (i + 1); }
        return V::makeI64(result);
    });
    m["primes_up_to"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t limit = toI(a[0]);
        if (limit < 2) return V::makeArray({});
        std::vector<bool> sieve(limit+1, true);
        sieve[0] = sieve[1] = false;
        for (int64_t i = 2; i*i <= limit; ++i)
            if (sieve[i]) for (int64_t j = i*i; j <= limit; j+=i) sieve[j]=false;
        std::vector<VR> primes;
        for (int64_t i = 2; i <= limit; ++i) if (sieve[i]) primes.push_back(V::makeI64(i));
        return V::makeArray(std::move(primes));
    });

    // Random number generation
    static std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    m["random"] = V::makeNative([](std::vector<VR>, VM&) -> VR {
        return V::makeF64(std::uniform_real_distribution<double>(0.0, 1.0)(rng));
    });
    m["random_int"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        int64_t lo = toI(a[0]), hi = toI(a[1]);
        return V::makeI64(std::uniform_int_distribution<int64_t>(lo, hi)(rng));
    });
    m["random_float"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        double lo = toD(a[0]), hi = toD(a[1]);
        return V::makeF64(std::uniform_real_distribution<double>(lo, hi)(rng));
    });
    m["shuffle"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (!a.empty() && a[0]->arrayData)
            std::shuffle(a[0]->arrayData->begin(), a[0]->arrayData->end(), rng);
        return nil();
    });
    m["sample"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.size() < 2 || !a[0]->arrayData || a[0]->arrayData->empty()) return V::makeNone();
        size_t n = std::min((size_t)std::max(0LL, toI(a[1])), a[0]->arrayData->size());
        std::vector<VR> pool = *a[0]->arrayData;
        std::shuffle(pool.begin(), pool.end(), rng);
        pool.resize(n);
        return V::makeArray(std::move(pool));
    });
    m["seed_rng"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        rng.seed((uint64_t)toI(a[0])); return nil();
    });
    return m;
}

// ─── std::io ─────────────────────────────────────────────────────────────────
static std::unordered_map<std::string, VR> makeIOModule() {
    std::unordered_map<std::string, VR> m;

    m["read_file"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->strData) return err("read_file: expected path");
        std::ifstream f(*args[0]->strData, std::ios::binary);
        if (!f) return err("read_file: cannot open " + *args[0]->strData);
        std::ostringstream ss; ss << f.rdbuf();
        return ok(V::makeString(ss.str()));
    });
    m["write_file"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return err("write_file: bad args");
        std::ofstream f(*args[0]->strData, std::ios::binary);
        if (!f) return err("write_file: cannot open " + *args[0]->strData);
        f << *args[1]->strData;
        return ok(nil());
    });
    m["append_file"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return err("append_file: bad args");
        std::ofstream f(*args[0]->strData, std::ios::binary | std::ios::app);
        if (!f) return err("append_file: cannot open " + *args[0]->strData);
        f << *args[1]->strData;
        return ok(nil());
    });
    m["file_exists"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->strData) return V::makeBool(false);
        return V::makeBool(std::filesystem::exists(*args[0]->strData));
    });
    m["file_size"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->strData) return err("file_size: expected path");
        std::error_code ec;
        auto sz = std::filesystem::file_size(*args[0]->strData, ec);
        if (ec) return err("file_size: " + ec.message());
        return ok(V::makeI64(sz));
    });
    m["list_dir"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->strData) return err("list_dir: expected path");
        std::vector<VR> entries;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(*args[0]->strData, ec))
            entries.push_back(V::makeString(e.path().string()));
        if (ec) return err("list_dir: " + ec.message());
        return ok(V::makeArray(std::move(entries)));
    });
    m["mkdir"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->strData) return err("mkdir: expected path");
        std::error_code ec;
        std::filesystem::create_directories(*args[0]->strData, ec);
        return ec ? err(ec.message()) : ok(nil());
    });
    m["remove"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->strData) return err("remove: expected path");
        std::error_code ec;
        std::filesystem::remove_all(*args[0]->strData, ec);
        return ec ? err(ec.message()) : ok(nil());
    });
    m["rename"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return err("rename: bad args");
        std::error_code ec;
        std::filesystem::rename(*args[0]->strData, *args[1]->strData, ec);
        return ec ? err(ec.message()) : ok(nil());
    });
    m["cwd"] = V::makeNative([](std::vector<VR>, VM&) -> VR {
        std::error_code ec;
        auto path = std::filesystem::current_path(ec);
        return ec ? err(ec.message()) : ok(V::makeString(path.string()));
    });
    m["read_lines"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->strData) return err("read_lines: expected path");
        std::ifstream f(*args[0]->strData);
        if (!f) return err("read_lines: cannot open " + *args[0]->strData);
        std::vector<VR> lines;
        std::string line;
        while (std::getline(f, line)) lines.push_back(V::makeString(line));
        return ok(V::makeArray(std::move(lines)));
    });
    return m;
}

// ─── std::time ───────────────────────────────────────────────────────────────
static std::unordered_map<std::string, VR> makeTimeModule() {
    std::unordered_map<std::string, VR> m;
    m["now_ms"] = V::makeNative([](std::vector<VR>, VM&) -> VR {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return V::makeI64(ms);
    });
    m["now_us"] = V::makeNative([](std::vector<VR>, VM&) -> VR {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return V::makeI64(us);
    });
    m["sleep_ms"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        std::this_thread::sleep_for(std::chrono::milliseconds(toI(args[0]))); return nil();
    });
    m["sleep_us"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        std::this_thread::sleep_for(std::chrono::microseconds(toI(args[0]))); return nil();
    });
    m["monotonic_ns"] = V::makeNative([](std::vector<VR>, VM&) -> VR {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return V::makeI64(ns);
    });
    return m;
}

// ─── std::regex ──────────────────────────────────────────────────────────────
static std::unordered_map<std::string, VR> makeRegexModule() {
    std::unordered_map<std::string, VR> m;
    m["match"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return V::makeBool(false);
        try {
            std::regex re(*args[1]->strData);
            return V::makeBool(std::regex_match(*args[0]->strData, re));
        } catch (...) { return err("regex::match: invalid pattern"); }
    });
    m["search"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return V::makeNone();
        try {
            std::regex re(*args[1]->strData);
            std::smatch sm;
            if (std::regex_search(*args[0]->strData, sm, re))
                return V::makeSome(V::makeString(sm[0].str()));
            return V::makeNone();
        } catch (...) { return V::makeNone(); }
    });
    m["find_all"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return V::makeArray({});
        try {
            std::regex re(*args[1]->strData);
            std::sregex_iterator it(args[0]->strData->begin(), args[0]->strData->end(), re);
            std::vector<VR> matches;
            for (std::sregex_iterator end; it != end; ++it) matches.push_back(V::makeString((*it)[0].str()));
            return V::makeArray(std::move(matches));
        } catch (...) { return V::makeArray({}); }
    });
    m["replace"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 3 || !args[0]->strData || !args[1]->strData || !args[2]->strData)
            return args.empty() ? V::makeString("") : args[0];
        try {
            std::regex re(*args[1]->strData);
            return V::makeString(std::regex_replace(*args[0]->strData, re, *args[2]->strData));
        } catch (...) { return args[0]; }
    });
    m["split"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return V::makeArray({});
        try {
            std::regex re(*args[1]->strData);
            std::sregex_token_iterator it(args[0]->strData->begin(), args[0]->strData->end(), re, -1);
            std::vector<VR> parts;
            for (std::sregex_token_iterator end; it != end; ++it)
                parts.push_back(V::makeString(it->str()));
            return V::makeArray(std::move(parts));
        } catch (...) { return V::makeArray({}); }
    });
    return m;
}

// ─── std::collections ────────────────────────────────────────────────────────
static std::unordered_map<std::string, VR> makeCollectionsModule() {
    std::unordered_map<std::string, VR> m;

    m["stack_new"] = V::makeNative([](std::vector<VR>, VM&) -> VR { return V::makeArray({}); });
    m["stack_push"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.size() < 2 || !a[0]->arrayData) return nil();
        a[0]->arrayData->push_back(a[1]); return nil();
    });
    m["stack_pop"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.empty() || !a[0]->arrayData || a[0]->arrayData->empty()) return V::makeNone();
        auto v = a[0]->arrayData->back(); a[0]->arrayData->pop_back(); return V::makeSome(v);
    });
    m["stack_peek"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.empty() || !a[0]->arrayData || a[0]->arrayData->empty()) return V::makeNone();
        return V::makeSome(a[0]->arrayData->back());
    });

    m["queue_new"] = V::makeNative([](std::vector<VR>, VM&) -> VR { return V::makeArray({}); });
    m["queue_enqueue"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.size() < 2 || !a[0]->arrayData) return nil();
        a[0]->arrayData->push_back(a[1]); return nil();
    });
    m["queue_dequeue"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.empty() || !a[0]->arrayData || a[0]->arrayData->empty()) return V::makeNone();
        auto v = a[0]->arrayData->front();
        a[0]->arrayData->erase(a[0]->arrayData->begin());
        return V::makeSome(v);
    });

    m["map_get"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.size() < 2 || !a[0]->mapData || !a[1]->strData) return V::makeNone();
        auto it = a[0]->mapData->find(*a[1]->strData);
        return it != a[0]->mapData->end() ? V::makeSome(it->second) : V::makeNone();
    });
    m["map_set"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.size() < 3 || !a[0]->mapData || !a[1]->strData) return nil();
        (*a[0]->mapData)[*a[1]->strData] = a[2]; return nil();
    });
    m["map_delete"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.size() < 2 || !a[0]->mapData || !a[1]->strData) return nil();
        a[0]->mapData->erase(*a[1]->strData); return nil();
    });
    m["map_keys"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.empty() || !a[0]->mapData) return V::makeArray({});
        std::vector<VR> keys;
        for (auto& [k, v] : *a[0]->mapData) keys.push_back(V::makeString(k));
        return V::makeArray(std::move(keys));
    });
    m["map_values"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.empty() || !a[0]->mapData) return V::makeArray({});
        std::vector<VR> vals;
        for (auto& [k, v] : *a[0]->mapData) vals.push_back(v);
        return V::makeArray(std::move(vals));
    });
    m["map_entries"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.empty() || !a[0]->mapData) return V::makeArray({});
        std::vector<VR> entries;
        for (auto& [k, v] : *a[0]->mapData)
            entries.push_back(V::makeArray({V::makeString(k), v}));
        return V::makeArray(std::move(entries));
    });
    m["map_merge"] = V::makeNative([](std::vector<VR> a, VM&) -> VR {
        if (a.size() < 2 || !a[0]->mapData || !a[1]->mapData) return a.empty() ? V::makeMap({}) : a[0];
        auto merged = *a[0]->mapData;
        for (auto& [k, v] : *a[1]->mapData) merged[k] = v;
        return V::makeMap(std::move(merged));
    });
    return m;
}

// ─── std::env ────────────────────────────────────────────────────────────────
static std::unordered_map<std::string, VR> makeEnvModule() {
    std::unordered_map<std::string, VR> m;
    m["get"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.empty() || !args[0]->strData) return V::makeNone();
#ifdef _WIN32
        char* val_buf = nullptr;
        size_t val_sz = 0;
        _dupenv_s(&val_buf, &val_sz, args[0]->strData->c_str());
        std::unique_ptr<char, decltype(&free)> _guard(val_buf, free);
        const char* val = val_buf;
#else
        const char* val = std::getenv(args[0]->strData->c_str());
#endif
        return val ? V::makeSome(V::makeString(val)) : V::makeNone();
    });
    m["set"] = V::makeNative([](std::vector<VR> args, VM&) -> VR {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return nil();
#ifdef _WIN32
        _putenv_s(args[0]->strData->c_str(), args[1]->strData->c_str());
#else
        ::setenv(args[0]->strData->c_str(), args[1]->strData->c_str(), 1);
#endif
        return nil();
    });
    m["args"] = V::makeNative([](std::vector<VR>, VM&) -> VR { return V::makeArray({}); });
    return m;
}

// ─── Main registration entrypoint ────────────────────────────────────────────
void registerAll(runtime::VM& vm) {
    vm.bindModule("algo",        makeAlgoModule());
    vm.bindModule("math",        makeMathModule());
    vm.bindModule("io",          makeIOModule());
    vm.bindModule("time",        makeTimeModule());
    vm.bindModule("re",          makeRegexModule());
    vm.bindModule("collections", makeCollectionsModule());
    vm.bindModule("env",         makeEnvModule());

    // Also make algo functions available globally (convenience)
    for (auto& [k, v] : makeAlgoModule()) {
        if (v->nativeFn)
            vm.bindNative(k, v->nativeFn);
    }
}

} // namespace stdlib
} // namespace fpp
