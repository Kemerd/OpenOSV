// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MockOfxHost.cpp - the strict OpenFX host of the tests (MockOfxHost.h).

#include "MockOfxHost.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace osv::ofxtest {

// ===========================================================================
//  PropertySet
// ===========================================================================

int PropertySet::Prop::size() const noexcept {
    switch (type) {
    case Type::String: return static_cast<int>(s.size());
    case Type::Int: return static_cast<int>(i.size());
    case Type::Double: return static_cast<int>(d.size());
    case Type::Pointer: return static_cast<int>(p.size());
    }
    return 0;
}

namespace {

/// The property `name` of type `type`, created (or retyped, with its old
/// values dropped) when needed, grown to hold `index`.
PropertySet::Prop& slot(PropertySet& set, const std::string& name, PropertySet::Type type, int index) {
    PropertySet::Prop& prop = set.props[name];
    if (prop.type != type) {
        prop = PropertySet::Prop{};
        prop.type = type;
    }
    const std::size_t need = static_cast<std::size_t>(index) + 1u;
    switch (type) {
    case PropertySet::Type::String: if (prop.s.size() < need) prop.s.resize(need); break;
    case PropertySet::Type::Int: if (prop.i.size() < need) prop.i.resize(need); break;
    case PropertySet::Type::Double: if (prop.d.size() < need) prop.d.resize(need); break;
    case PropertySet::Type::Pointer: if (prop.p.size() < need) prop.p.resize(need); break;
    }
    return prop;
}

}  // namespace

void PropertySet::setString(const std::string& name, const std::string& value, int index) {
    slot(*this, name, Type::String, index).s[static_cast<std::size_t>(index)] = value;
}
void PropertySet::setInt(const std::string& name, int value, int index) {
    slot(*this, name, Type::Int, index).i[static_cast<std::size_t>(index)] = value;
}
void PropertySet::setDouble(const std::string& name, double value, int index) {
    slot(*this, name, Type::Double, index).d[static_cast<std::size_t>(index)] = value;
}
void PropertySet::setPointer(const std::string& name, void* value, int index) {
    slot(*this, name, Type::Pointer, index).p[static_cast<std::size_t>(index)] = value;
}
void PropertySet::setDoubles(const std::string& name, const std::vector<double>& values) {
    Prop& p = props[name];
    p = Prop{};
    p.type = Type::Double;
    p.d = values;
}
void PropertySet::setInts(const std::string& name, const std::vector<int>& values) {
    Prop& p = props[name];
    p = Prop{};
    p.type = Type::Int;
    p.i = values;
}

bool PropertySet::has(const std::string& name) const { return props.count(name) != 0; }

int PropertySet::dimension(const std::string& name) const {
    auto it = props.find(name);
    return it == props.end() ? -1 : it->second.size();
}

std::string PropertySet::getString(const std::string& name, int index) const {
    auto it = props.find(name);
    if (it == props.end() || it->second.type != Type::String || index >= it->second.size()) {
        return {};
    }
    return it->second.s[static_cast<std::size_t>(index)];
}
int PropertySet::getInt(const std::string& name, int index) const {
    auto it = props.find(name);
    if (it == props.end() || it->second.type != Type::Int || index >= it->second.size()) {
        return 0;
    }
    return it->second.i[static_cast<std::size_t>(index)];
}
double PropertySet::getDouble(const std::string& name, int index) const {
    auto it = props.find(name);
    if (it == props.end() || it->second.type != Type::Double || index >= it->second.size()) {
        return 0.0;
    }
    return it->second.d[static_cast<std::size_t>(index)];
}
void* PropertySet::getPointer(const std::string& name, int index) const {
    auto it = props.find(name);
    if (it == props.end() || it->second.type != Type::Pointer || index >= it->second.size()) {
        return nullptr;
    }
    return it->second.p[static_cast<std::size_t>(index)];
}
std::vector<std::string> PropertySet::getStrings(const std::string& name) const {
    auto it = props.find(name);
    if (it == props.end() || it->second.type != Type::String) {
        return {};
    }
    return it->second.s;
}

// ===========================================================================
//  Param / ParamSet
// ===========================================================================

bool Param::isDouble() const noexcept { return type == kOfxParamTypeDouble; }
bool Param::isIntLike() const noexcept {
    return type == kOfxParamTypeInteger || type == kOfxParamTypeChoice || type == kOfxParamTypeBoolean;
}
bool Param::isString() const noexcept { return type == kOfxParamTypeString; }

double Param::doubleAt(double time) const {
    if (dkeys.empty()) {
        return d;
    }
    // Held before the first and after the last key, linear in between.
    if (time <= dkeys.begin()->first) {
        return dkeys.begin()->second;
    }
    if (time >= dkeys.rbegin()->first) {
        return dkeys.rbegin()->second;
    }
    auto hi = dkeys.upper_bound(time);
    auto lo = std::prev(hi);
    const double u = (time - lo->first) / (hi->first - lo->first);
    return lo->second + (hi->second - lo->second) * u;
}

int Param::intAt(double time) const {
    if (ikeys.empty()) {
        return i;
    }
    // Stepped: the last key at or before the time.
    auto it = ikeys.upper_bound(time);
    if (it == ikeys.begin()) {
        return it->second;
    }
    return std::prev(it)->second;
}

std::vector<double> Param::keyTimes() const {
    std::vector<double> times;
    if (isDouble()) {
        for (const auto& k : dkeys) times.push_back(k.first);
    } else {
        for (const auto& k : ikeys) times.push_back(k.first);
    }
    return times;
}

Param* ParamSet::find(const std::string& name) {
    for (auto& p : params) {
        if (p->name == name) {
            return p.get();
        }
    }
    return nullptr;
}
const Param* ParamSet::find(const std::string& name) const {
    for (const auto& p : params) {
        if (p->name == name) {
            return p.get();
        }
    }
    return nullptr;
}

Param& ParamSet::add(const std::string& name, const std::string& type) {
    auto p = std::make_unique<Param>();
    p->name = name;
    p->type = type;
    p->props.setString(kOfxPropName, name);
    p->props.setString(kOfxPropType, kOfxTypeParameter);
    p->props.setString(kOfxParamPropType, type);
    params.push_back(std::move(p));
    return *params.back();
}

void ParamSet::instantiateFrom(const ParamSet& descriptor) {
    params.clear();
    for (const auto& src : descriptor.params) {
        Param& p = add(src->name, src->type);
        p.props = src->props;
        // Start from the declared default, of the type the kind stores.
        if (p.isDouble()) {
            p.d = p.props.getDouble(kOfxParamPropDefault);
        } else if (p.isIntLike()) {
            p.i = p.props.getInt(kOfxParamPropDefault);
        } else if (p.isString()) {
            p.s = p.props.getString(kOfxParamPropDefault);
        }
    }
}

Clip* Effect::clip(const std::string& name) {
    auto it = clips.find(name);
    return it == clips.end() ? nullptr : it->second.get();
}

// ===========================================================================
//  The property suite
// ===========================================================================

namespace {

using Type = PropertySet::Type;

/// The property of `handle` called `name`, or null (with the status to
/// return in `status`) when it is missing or of another type.
PropertySet::Prop* lookup(OfxPropertySetHandle handle, const char* name, Type type, int index, OfxStatus& status) {
    status = kOfxStatOK;
    if (!handle || !name) {
        status = kOfxStatErrBadHandle;
        return nullptr;
    }
    PropertySet* set = PropertySet::from(handle);
    auto it = set->props.find(name);
    if (it == set->props.end()) {
        status = kOfxStatErrUnknown;
        return nullptr;
    }
    if (it->second.type != type) {
        status = kOfxStatErrValue;
        return nullptr;
    }
    if (index < 0 || index >= it->second.size()) {
        status = kOfxStatErrBadIndex;
        return nullptr;
    }
    return &it->second;
}

OfxStatus propSetPointer(OfxPropertySetHandle h, const char* name, int index, void* value) {
    if (!h || !name || index < 0) return kOfxStatErrBadHandle;
    PropertySet::from(h)->setPointer(name, value, index);
    return kOfxStatOK;
}
OfxStatus propSetString(OfxPropertySetHandle h, const char* name, int index, const char* value) {
    if (!h || !name || index < 0 || !value) return kOfxStatErrBadHandle;
    PropertySet::from(h)->setString(name, value, index);
    return kOfxStatOK;
}
OfxStatus propSetDouble(OfxPropertySetHandle h, const char* name, int index, double value) {
    if (!h || !name || index < 0) return kOfxStatErrBadHandle;
    PropertySet::from(h)->setDouble(name, value, index);
    return kOfxStatOK;
}
OfxStatus propSetInt(OfxPropertySetHandle h, const char* name, int index, int value) {
    if (!h || !name || index < 0) return kOfxStatErrBadHandle;
    PropertySet::from(h)->setInt(name, value, index);
    return kOfxStatOK;
}
OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* name, int count, void* const* value) {
    if (!h || !name || count <= 0 || !value) return kOfxStatErrBadHandle;
    for (int k = 0; k < count; ++k) PropertySet::from(h)->setPointer(name, value[k], k);
    return kOfxStatOK;
}
OfxStatus propSetStringN(OfxPropertySetHandle h, const char* name, int count, const char* const* value) {
    if (!h || !name || count <= 0 || !value) return kOfxStatErrBadHandle;
    for (int k = 0; k < count; ++k) PropertySet::from(h)->setString(name, value[k] ? value[k] : "", k);
    return kOfxStatOK;
}
OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* name, int count, const double* value) {
    if (!h || !name || count <= 0 || !value) return kOfxStatErrBadHandle;
    for (int k = 0; k < count; ++k) PropertySet::from(h)->setDouble(name, value[k], k);
    return kOfxStatOK;
}
OfxStatus propSetIntN(OfxPropertySetHandle h, const char* name, int count, const int* value) {
    if (!h || !name || count <= 0 || !value) return kOfxStatErrBadHandle;
    for (int k = 0; k < count; ++k) PropertySet::from(h)->setInt(name, value[k], k);
    return kOfxStatOK;
}

OfxStatus propGetPointer(OfxPropertySetHandle h, const char* name, int index, void** value) {
    OfxStatus st;
    PropertySet::Prop* p = lookup(h, name, Type::Pointer, index, st);
    if (!p || !value) return p ? kOfxStatErrBadHandle : st;
    *value = p->p[static_cast<std::size_t>(index)];
    return kOfxStatOK;
}
OfxStatus propGetString(OfxPropertySetHandle h, const char* name, int index, char** value) {
    OfxStatus st;
    PropertySet::Prop* p = lookup(h, name, Type::String, index, st);
    if (!p || !value) return p ? kOfxStatErrBadHandle : st;
    // The host owns the characters; they stay valid until the property changes.
    *value = const_cast<char*>(p->s[static_cast<std::size_t>(index)].c_str());
    return kOfxStatOK;
}
OfxStatus propGetDouble(OfxPropertySetHandle h, const char* name, int index, double* value) {
    OfxStatus st;
    PropertySet::Prop* p = lookup(h, name, Type::Double, index, st);
    if (!p || !value) return p ? kOfxStatErrBadHandle : st;
    *value = p->d[static_cast<std::size_t>(index)];
    return kOfxStatOK;
}
OfxStatus propGetInt(OfxPropertySetHandle h, const char* name, int index, int* value) {
    OfxStatus st;
    PropertySet::Prop* p = lookup(h, name, Type::Int, index, st);
    if (!p || !value) return p ? kOfxStatErrBadHandle : st;
    *value = p->i[static_cast<std::size_t>(index)];
    return kOfxStatOK;
}
OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* name, int count, void** value) {
    for (int k = 0; k < count; ++k) {
        const OfxStatus st = propGetPointer(h, name, k, value + k);
        if (st != kOfxStatOK) return st;
    }
    return kOfxStatOK;
}
OfxStatus propGetStringN(OfxPropertySetHandle h, const char* name, int count, char** value) {
    for (int k = 0; k < count; ++k) {
        const OfxStatus st = propGetString(h, name, k, value + k);
        if (st != kOfxStatOK) return st;
    }
    return kOfxStatOK;
}
OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* name, int count, double* value) {
    for (int k = 0; k < count; ++k) {
        const OfxStatus st = propGetDouble(h, name, k, value + k);
        if (st != kOfxStatOK) return st;
    }
    return kOfxStatOK;
}
OfxStatus propGetIntN(OfxPropertySetHandle h, const char* name, int count, int* value) {
    for (int k = 0; k < count; ++k) {
        const OfxStatus st = propGetInt(h, name, k, value + k);
        if (st != kOfxStatOK) return st;
    }
    return kOfxStatOK;
}
OfxStatus propReset(OfxPropertySetHandle h, const char* name) {
    if (!h || !name) return kOfxStatErrBadHandle;
    return PropertySet::from(h)->props.erase(name) ? kOfxStatOK : kOfxStatErrUnknown;
}
OfxStatus propGetDimension(OfxPropertySetHandle h, const char* name, int* count) {
    if (!h || !name || !count) return kOfxStatErrBadHandle;
    const int n = PropertySet::from(h)->dimension(name);
    if (n < 0) return kOfxStatErrUnknown;
    *count = n;
    return kOfxStatOK;
}

const OfxPropertySuiteV1 kPropertySuite = {
    propSetPointer,  propSetString,  propSetDouble,  propSetInt,    propSetPointerN, propSetStringN,
    propSetDoubleN,  propSetIntN,    propGetPointer, propGetString, propGetDouble,   propGetInt,
    propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN,   propReset,       propGetDimension,
};

// ===========================================================================
//  The parameter suite
// ===========================================================================

Param* paramOf(OfxParamHandle h) { return reinterpret_cast<Param*>(h); }

OfxStatus paramDefine(OfxParamSetHandle set, const char* type, const char* name, OfxPropertySetHandle* props) {
    if (!set || !type || !name) return kOfxStatErrBadHandle;
    ParamSet* ps = ParamSet::from(set);
    if (ps->find(name)) return kOfxStatErrExists;
    Param& p = ps->add(name, type);
    if (props) *props = p.props.handle();
    return kOfxStatOK;
}
OfxStatus paramGetHandle(OfxParamSetHandle set, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props) {
    if (!set || !name) return kOfxStatErrBadHandle;
    Param* p = ParamSet::from(set)->find(name);
    if (!p) return kOfxStatErrUnknown;
    if (param) *param = reinterpret_cast<OfxParamHandle>(p);
    if (props) *props = p->props.handle();
    return kOfxStatOK;
}
OfxStatus paramSetGetPropertySet(OfxParamSetHandle set, OfxPropertySetHandle* props) {
    if (!set || !props) return kOfxStatErrBadHandle;
    *props = ParamSet::from(set)->props.handle();
    return kOfxStatOK;
}
OfxStatus paramGetPropertySet(OfxParamHandle param, OfxPropertySetHandle* props) {
    if (!param || !props) return kOfxStatErrBadHandle;
    *props = paramOf(param)->props.handle();
    return kOfxStatOK;
}

/// Read a parameter's value at `time` into the next vararg pointer.
OfxStatus readValue(Param* p, double time, va_list ap) {
    if (p->isDouble()) {
        double* out = va_arg(ap, double*);
        if (!out) return kOfxStatErrBadHandle;
        *out = p->doubleAt(time);
        return kOfxStatOK;
    }
    if (p->isIntLike()) {
        int* out = va_arg(ap, int*);
        if (!out) return kOfxStatErrBadHandle;
        *out = p->intAt(time);
        return kOfxStatOK;
    }
    if (p->isString()) {
        char** out = va_arg(ap, char**);
        if (!out) return kOfxStatErrBadHandle;
        *out = const_cast<char*>(p->s.c_str());
        return kOfxStatOK;
    }
    return kOfxStatErrUnsupported;  // groups, pages, push buttons
}

OfxStatus paramGetValue(OfxParamHandle param, ...) {
    if (!param) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, param);
    const OfxStatus st = readValue(paramOf(param), 0.0, ap);
    va_end(ap);
    return st;
}
OfxStatus paramGetValueAtTime(OfxParamHandle param, OfxTime time, ...) {
    if (!param) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    const OfxStatus st = readValue(paramOf(param), time, ap);
    va_end(ap);
    return st;
}
OfxStatus paramGetDerivative(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus paramGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }

OfxStatus paramSetValue(OfxParamHandle param, ...) {
    if (!param) return kOfxStatErrBadHandle;
    Param* p = paramOf(param);
    va_list ap;
    va_start(ap, param);
    OfxStatus st = kOfxStatOK;
    if (p->isDouble()) {
        p->d = va_arg(ap, double);
    } else if (p->isIntLike()) {
        p->i = va_arg(ap, int);
    } else if (p->isString()) {
        const char* s = va_arg(ap, const char*);
        p->s = s ? s : "";
    } else {
        st = kOfxStatErrUnsupported;
    }
    va_end(ap);
    if (st == kOfxStatOK) ++p->pluginWrites;
    return st;
}
OfxStatus paramSetValueAtTime(OfxParamHandle param, OfxTime time, ...) {
    if (!param) return kOfxStatErrBadHandle;
    Param* p = paramOf(param);
    va_list ap;
    va_start(ap, time);
    OfxStatus st = kOfxStatOK;
    if (p->isDouble()) {
        p->dkeys[time] = va_arg(ap, double);
    } else if (p->isIntLike()) {
        p->ikeys[time] = va_arg(ap, int);
    } else {
        st = kOfxStatErrUnsupported;
    }
    va_end(ap);
    if (st == kOfxStatOK) ++p->pluginWrites;
    return st;
}
OfxStatus paramGetNumKeys(OfxParamHandle param, unsigned int* n) {
    if (!param || !n) return kOfxStatErrBadHandle;
    *n = static_cast<unsigned int>(paramOf(param)->keyTimes().size());
    return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle param, unsigned int nth, OfxTime* time) {
    if (!param || !time) return kOfxStatErrBadHandle;
    const std::vector<double> t = paramOf(param)->keyTimes();
    if (nth >= t.size()) return kOfxStatErrBadIndex;
    *time = t[nth];
    return kOfxStatOK;
}
OfxStatus paramGetKeyIndex(OfxParamHandle param, OfxTime time, int direction, int* index) {
    if (!param || !index) return kOfxStatErrBadHandle;
    *index = -1;
    const std::vector<double> t = paramOf(param)->keyTimes();
    constexpr double kEps = 1e-6;
    if (direction == 0) {
        for (std::size_t k = 0; k < t.size(); ++k) {
            if (std::fabs(t[k] - time) < kEps) { *index = static_cast<int>(k); return kOfxStatOK; }
        }
        return kOfxStatFailed;
    }
    if (direction < 0) {
        for (int k = static_cast<int>(t.size()) - 1; k >= 0; --k) {
            if (t[static_cast<std::size_t>(k)] < time - kEps) { *index = k; return kOfxStatOK; }
        }
        return kOfxStatFailed;
    }
    for (std::size_t k = 0; k < t.size(); ++k) {
        if (t[k] > time + kEps) { *index = static_cast<int>(k); return kOfxStatOK; }
    }
    return kOfxStatFailed;
}
OfxStatus paramDeleteKey(OfxParamHandle param, OfxTime time) {
    if (!param) return kOfxStatErrBadHandle;
    Param* p = paramOf(param);
    return (p->dkeys.erase(time) + p->ikeys.erase(time)) ? kOfxStatOK : kOfxStatErrBadIndex;
}
OfxStatus paramDeleteAllKeys(OfxParamHandle param) {
    if (!param) return kOfxStatErrBadHandle;
    paramOf(param)->dkeys.clear();
    paramOf(param)->ikeys.clear();
    return kOfxStatOK;
}
OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatErrUnsupported; }
OfxStatus paramEditBegin(OfxParamSetHandle set, const char*) {
    if (!set) return kOfxStatErrBadHandle;
    ++ParamSet::from(set)->editDepth;
    ++ParamSet::from(set)->editGroups;
    return kOfxStatOK;
}
OfxStatus paramEditEnd(OfxParamSetHandle set) {
    if (!set) return kOfxStatErrBadHandle;
    --ParamSet::from(set)->editDepth;
    return kOfxStatOK;
}

const OfxParameterSuiteV1 kParameterSuite = {
    paramDefine,      paramGetHandle,     paramSetGetPropertySet, paramGetPropertySet, paramGetValue,
    paramGetValueAtTime, paramGetDerivative, paramGetIntegral,    paramSetValue,       paramSetValueAtTime,
    paramGetNumKeys,  paramGetKeyTime,    paramGetKeyIndex,       paramDeleteKey,      paramDeleteAllKeys,
    paramCopy,        paramEditBegin,     paramEditEnd,
};

// ===========================================================================
//  The image effect suite
// ===========================================================================

OfxStatus getPropertySet(OfxImageEffectHandle effect, OfxPropertySetHandle* props) {
    if (!effect || !props) return kOfxStatErrBadHandle;
    *props = Effect::from(effect)->props.handle();
    return kOfxStatOK;
}
OfxStatus getParamSet(OfxImageEffectHandle effect, OfxParamSetHandle* params) {
    if (!effect || !params) return kOfxStatErrBadHandle;
    *params = Effect::from(effect)->params.handle();
    return kOfxStatOK;
}
OfxStatus clipDefine(OfxImageEffectHandle effect, const char* name, OfxPropertySetHandle* props) {
    if (!effect || !name) return kOfxStatErrBadHandle;
    Effect* e = Effect::from(effect);
    if (e->clips.count(name)) return kOfxStatErrExists;
    auto clip = std::make_unique<Clip>();
    clip->name = name;
    clip->props.setString(kOfxPropName, name);
    clip->props.setString(kOfxPropType, kOfxTypeClip);
    if (props) *props = clip->props.handle();
    e->clipOrder.push_back(name);
    e->clips[name] = std::move(clip);
    return kOfxStatOK;
}
OfxStatus clipGetHandle(OfxImageEffectHandle effect, const char* name, OfxImageClipHandle* clip,
                        OfxPropertySetHandle* props) {
    if (!effect || !name) return kOfxStatErrBadHandle;
    Clip* c = Effect::from(effect)->clip(name);
    if (!c) return kOfxStatErrUnknown;
    if (clip) *clip = c->handle();
    if (props) *props = c->props.handle();
    return kOfxStatOK;
}
OfxStatus clipGetPropertySet(OfxImageClipHandle clip, OfxPropertySetHandle* props) {
    if (!clip || !props) return kOfxStatErrBadHandle;
    *props = Clip::from(clip)->props.handle();
    return kOfxStatOK;
}
OfxStatus clipGetImage(OfxImageClipHandle clip, OfxTime time, const OfxRectD*, OfxPropertySetHandle* image) {
    if (!clip || !image) return kOfxStatErrBadHandle;
    Clip* c = Clip::from(clip);
    if (!c->provide) return kOfxStatFailed;
    // The image IS its property set: the handle the plug-in gets back is the
    // one it releases.
    auto* img = new PropertySet();
    img->setString(kOfxPropType, kOfxTypeImage);
    if (!c->provide(time, *img)) {
        delete img;
        return kOfxStatFailed;
    }
    ++MockHost::instance().imagesOut;
    ++MockHost::instance().imagesTotal;
    *image = img->handle();
    return kOfxStatOK;
}
OfxStatus clipReleaseImage(OfxPropertySetHandle image) {
    if (!image) return kOfxStatErrBadHandle;
    delete PropertySet::from(image);
    --MockHost::instance().imagesOut;
    return kOfxStatOK;
}
OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle clip, OfxTime, OfxRectD* bounds) {
    if (!clip || !bounds) return kOfxStatErrBadHandle;
    *bounds = Clip::from(clip)->rod;
    return kOfxStatOK;
}
int abortFn(OfxImageEffectHandle) { return 0; }
OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t, OfxImageMemoryHandle*) { return kOfxStatErrUnsupported; }
OfxStatus imageMemoryFree(OfxImageMemoryHandle) { return kOfxStatErrUnsupported; }
OfxStatus imageMemoryLock(OfxImageMemoryHandle, void**) { return kOfxStatErrUnsupported; }
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle) { return kOfxStatErrUnsupported; }

const OfxImageEffectSuiteV1 kImageEffectSuite = {
    getPropertySet, getParamSet,     clipDefine,        clipGetHandle,    clipGetPropertySet,
    clipGetImage,   clipReleaseImage, clipGetRegionOfDefinition, abortFn, imageMemoryAlloc,
    imageMemoryFree, imageMemoryLock, imageMemoryUnlock,
};

// ===========================================================================
//  The message suite
// ===========================================================================

OfxStatus message(void*, const char* type, const char*, const char* format, ...) {
    char buffer[4096] = {};
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buffer, sizeof(buffer), format ? format : "", ap);
    va_end(ap);
    MockHost::instance().messages.push_back(std::string(type ? type : "?") + ": " + buffer);
    return kOfxStatOK;
}

const OfxMessageSuiteV1 kMessageSuite = {message};

const void* fetchSuite(OfxPropertySetHandle, const char* name, int version) {
    if (!name) return nullptr;
    if (std::strcmp(name, kOfxPropertySuite) == 0 && version == 1) return &kPropertySuite;
    if (std::strcmp(name, kOfxParameterSuite) == 0 && version == 1) return &kParameterSuite;
    if (std::strcmp(name, kOfxImageEffectSuite) == 0 && version == 1) return &kImageEffectSuite;
    if (std::strcmp(name, kOfxMessageSuite) == 0 && version == 1) return &kMessageSuite;
    return nullptr;
}

}  // namespace

// ===========================================================================
//  MockHost
// ===========================================================================

MockHost& MockHost::instance() {
    static MockHost host;
    return host;
}

MockHost::MockHost() {
    hostProps.setString(kOfxPropName, "OpenOSV.MockOfxHost");
    hostProps.setString(kOfxPropLabel, "OpenOSV mock OpenFX host");
    hostProps.setInts(kOfxPropAPIVersion, {1, 5});
    m_host.host = hostProps.handle();
    m_host.fetchSuite = &fetchSuite;
}

// ===========================================================================
//  LoadedModule
// ===========================================================================

LoadedModule::LoadedModule(const std::wstring& path) {
    // LOAD_WITH_ALTERED_SEARCH_PATH: the module's own imports resolve from
    // its folder, the way a host that loads by full path behaves.
    m_module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_module) {
        return;
    }
    m_getCount = reinterpret_cast<int (*)()>(::GetProcAddress(m_module, "OfxGetNumberOfPlugins"));
    m_getPlugin = reinterpret_cast<OfxPlugin* (*)(int)>(::GetProcAddress(m_module, "OfxGetPlugin"));
    m_setHost = reinterpret_cast<OfxStatus (*)(const OfxHost*)>(::GetProcAddress(m_module, "OfxSetHost"));
    if (m_setHost) {
        m_setHost(MockHost::instance().ofxHost());
    }
}

LoadedModule::~LoadedModule() {
    // Deliberately NOT unloaded: the module keeps worker threads (the thread
    // pool, the importer's analysis workers) and a module unloaded under a
    // live thread crashes the test process on exit.  The process ending
    // releases it, exactly as when a host quits.
}

int LoadedModule::pluginCount() const { return m_getCount ? m_getCount() : 0; }

OfxPlugin* LoadedModule::plugin(int index) const { return m_getPlugin ? m_getPlugin(index) : nullptr; }

OfxPlugin* LoadedModule::plugin(const std::string& identifier) const {
    for (int i = 0; i < pluginCount(); ++i) {
        OfxPlugin* p = plugin(i);
        if (p && p->pluginIdentifier && identifier == p->pluginIdentifier) {
            return p;
        }
    }
    return nullptr;
}

// ===========================================================================
//  PluginHarness
// ===========================================================================

PluginHarness::PluginHarness(OfxPlugin* plugin) : m_plugin(plugin) {
    if (m_plugin && m_plugin->setHost) {
        m_plugin->setHost(MockHost::instance().ofxHost());
    }
}

OfxStatus PluginHarness::action(const char* name, void* handle, PropertySet* inArgs, PropertySet* outArgs) {
    if (!m_plugin || !m_plugin->mainEntry) {
        return kOfxStatErrBadHandle;
    }
    return m_plugin->mainEntry(name, handle, inArgs ? inArgs->handle() : nullptr,
                               outArgs ? outArgs->handle() : nullptr);
}

OfxStatus PluginHarness::load() { return action(kOfxActionLoad, nullptr, nullptr, nullptr); }
OfxStatus PluginHarness::unload() { return action(kOfxActionUnload, nullptr, nullptr, nullptr); }

OfxStatus PluginHarness::describe() {
    descriptor = Effect{};
    descriptor.props.setString(kOfxPropType, kOfxTypeImageEffect);
    return action(kOfxActionDescribe, descriptor.handle(), nullptr, nullptr);
}

std::unique_ptr<Effect> PluginHarness::describeInContext(const std::string& context, OfxStatus* status) {
    auto effect = std::make_unique<Effect>();
    effect->props = descriptor.props;
    effect->props.setString(kOfxImageEffectPropContext, context);
    PropertySet in;
    in.setString(kOfxImageEffectPropContext, context);
    const OfxStatus st = action(kOfxImageEffectActionDescribeInContext, effect->handle(), &in, nullptr);
    if (status) *status = st;
    return effect;
}

std::unique_ptr<Effect> PluginHarness::createInstance(const std::string& context, int projectW, int projectH,
                                                      double fps, OfxStatus* status) {
    OfxStatus st = kOfxStatOK;
    std::unique_ptr<Effect> ctx = describeInContext(context, &st);
    auto effect = std::make_unique<Effect>();
    effect->props = ctx->props;
    effect->props.setString(kOfxPropType, kOfxTypeImageEffectInstance);
    effect->props.setString(kOfxImageEffectPropContext, context);
    effect->props.setPointer(kOfxPropInstanceData, nullptr);
    effect->props.setDoubles(kOfxImageEffectPropProjectSize, {static_cast<double>(projectW), static_cast<double>(projectH)});
    effect->props.setDoubles(kOfxImageEffectPropProjectExtent, {static_cast<double>(projectW), static_cast<double>(projectH)});
    effect->props.setDoubles(kOfxImageEffectPropProjectOffset, {0.0, 0.0});
    effect->props.setDouble(kOfxImageEffectPropProjectPixelAspectRatio, 1.0);
    effect->props.setDouble(kOfxImageEffectPropFrameRate, fps);
    effect->props.setDouble(kOfxImageEffectInstancePropEffectDuration, 1000.0);
    effect->props.setInt(kOfxPropIsInteractive, 1);
    effect->params.instantiateFrom(ctx->params);
    // Clips: the context descriptor's, as instances with a frame the size of
    // the project (the tests fill `provide` and `rod`).
    for (const std::string& name : ctx->clipOrder) {
        auto clip = std::make_unique<Clip>();
        clip->name = name;
        clip->props = ctx->clips[name]->props;
        clip->props.setString(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
        clip->props.setString(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
        clip->props.setDouble(kOfxImageEffectPropFrameRate, fps);
        clip->props.setDoubles(kOfxImageEffectPropFrameRange, {0.0, 999.0});
        clip->props.setInt(kOfxImageClipPropConnected, 1);
        clip->rod = OfxRectD{0.0, 0.0, static_cast<double>(projectW), static_cast<double>(projectH)};
        effect->clipOrder.push_back(name);
        effect->clips[name] = std::move(clip);
    }
    if (st == kOfxStatOK || st == kOfxStatReplyDefault) {
        st = action(kOfxActionCreateInstance, effect->handle(), nullptr, nullptr);
    }
    if (status) *status = st;
    return effect;
}

OfxStatus PluginHarness::destroyInstance(Effect& effect) {
    return action(kOfxActionDestroyInstance, effect.handle(), nullptr, nullptr);
}

OfxStatus PluginHarness::instanceChanged(Effect& effect, const std::string& param, const std::string& reason,
                                         double time) {
    PropertySet in;
    in.setString(kOfxPropType, kOfxTypeParameter);
    in.setString(kOfxPropName, param);
    in.setString(kOfxPropChangeReason, reason);
    in.setDouble(kOfxPropTime, time);
    in.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    PropertySet out;
    return action(kOfxActionInstanceChanged, effect.handle(), &in, &out);
}

OfxStatus PluginHarness::render(Effect& effect, const RenderArgs& args) {
    PropertySet in;
    in.setDouble(kOfxPropTime, args.time);
    in.setString(kOfxImageEffectPropFieldToRender, kOfxImageFieldNone);
    in.setInts(kOfxImageEffectPropRenderWindow, {args.window.x1, args.window.y1, args.window.x2, args.window.y2});
    in.setDoubles(kOfxImageEffectPropRenderScale, {args.scaleX, args.scaleY});
    in.setInt(kOfxImageEffectPropSequentialRenderStatus, 0);
    in.setInt(kOfxImageEffectPropInteractiveRenderStatus, args.interactive ? 1 : 0);
    in.setInt(kOfxImageEffectPropRenderQualityDraft, args.draft ? 1 : 0);
    if (args.setCudaProps) {
        in.setInt(kOfxImageEffectPropCudaEnabled, args.cuda ? 1 : 0);
        in.setPointer(kOfxImageEffectPropCudaStream, args.stream);
    }
    PropertySet out;
    return action(kOfxImageEffectActionRender, effect.handle(), &in, &out);
}

OfxStatus PluginHarness::regionsOfInterest(Effect& effect, double time, const OfxRectD& region,
                                           const std::string& clip, OfxRectD& roi) {
    PropertySet in;
    in.setDouble(kOfxPropTime, time);
    in.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    in.setDoubles(kOfxImageEffectPropRegionOfInterest, {region.x1, region.y1, region.x2, region.y2});
    PropertySet out;
    const std::string name = "OfxImageClipPropRoI_" + clip;
    // Initialised to the default RoI, as the specification requires.
    out.setDoubles(name, {region.x1, region.y1, region.x2, region.y2});
    const OfxStatus st = action(kOfxImageEffectActionGetRegionsOfInterest, effect.handle(), &in, &out);
    roi = OfxRectD{out.getDouble(name, 0), out.getDouble(name, 1), out.getDouble(name, 2), out.getDouble(name, 3)};
    return st;
}

}  // namespace osv::ofxtest
