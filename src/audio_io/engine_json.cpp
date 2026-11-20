// AudioEngine::snapshot() and the UI JSON (levels ~15x/s, full configuration on version change).
#include <algorithm>
#include <charconv>
#include <cmath>

#include "audio_io/engine_impl.hpp"
#include "audio_io/json_util.hpp"
#include "core/json.hpp"

namespace hb::audio_io {

using json_util::dbNum;
using json_util::num;
using json_util::quote;

EngineSnapshot AudioEngine::snapshot() const {
    Impl& P = *p_;
    EngineSnapshot s;
    s.stats = stats();  // takes the control lock itself
    s.devices = P.enumerator.devices();
    s.effects = hb::audio::EffectRegistry::instance().list();
    std::lock_guard lock(P.ctl);
    s.version = P.version.load();
    for (int i = 0; i < P.n; ++i) {
        const StripCtl& c = P.strips[static_cast<std::size_t>(i)];
        const StripRT& r = *P.rt[static_cast<std::size_t>(i)];
        StripSnapshot st;
        st.index = i;
        st.name = c.name;
        st.inputKind = c.kind;
        st.inputId = c.kind == InputKind::Device ? c.deviceId : c.kind == InputKind::External ? c.externalId : "";
        if (c.kind == InputKind::Device) {
            if (auto d = P.enumerator.find(c.deviceId)) st.inputName = d->name;
            if (c.capture) {
                const auto ss = c.capture->status();
                st.inputState = streamStateName(ss.state);
                st.inputMessage = ss.message;
                st.underruns = ss.underruns;
                st.overruns = ss.overruns;
                if (ss.state == StreamState::Idle) st.inputState = "connecting";
            }
        } else if (c.kind == InputKind::External) {
            st.inputName = c.externalId;
            const bool ok = c.node && c.node->source;
            st.inputState = ok ? "online" : "error";
            st.inputMessage = ok ? "Fonte externa conectada." : "Fonte externa não disponível.";
        } else {
            st.inputState = "off";
            st.inputMessage = "Sem entrada.";
        }
        st.trimDb = c.trimDb;
        st.faderDb = c.faderDb;
        st.mute = c.mute;
        st.solo = c.solo;
        st.pan = c.pan;
        st.phase = c.phase;
        st.mono = c.mono;
        st.delayMs = c.delayMs;
        st.quality = c.quality;
        st.toProgram = c.toProgram;
        st.toMonitor = c.toMonitor;
        for (const auto& slot : c.chain->slots) {
            EffectSnapshot e;
            e.type = slot->typeId;
            e.name = slot->name.empty() ? slot->typeId : slot->name;
            e.bypass = slot->bypass.load();
            e.missing = slot->missing || !slot->fx;
            e.gainReductionDb = slot->gr.load();
            if (slot->fx) {
                e.info = slot->fx->params();
                for (const auto& pi : e.info) e.values.push_back(slot->fx->getParam(pi.id));
            }
            st.effects.push_back(std::move(e));
        }
        st.peakDb[0] = r.pub[0].load();
        st.peakDb[1] = r.pub[1].load();
        st.holdDb[0] = r.pub[2].load();
        st.holdDb[1] = r.pub[3].load();
        st.rmsDb[0] = r.pub[4].load();
        st.rmsDb[1] = r.pub[5].load();
        st.lufsShort = r.pub[6].load();
        st.gainReductionDb = r.pub[7].load();
        s.strips.push_back(std::move(st));
    }
    s.master.faderDb = P.master.faderDb;
    s.master.limiter = P.master.limiter;
    s.master.ceilingDb = P.master.ceilingDb;
    s.master.levels = P.mixer.programLevels();
    s.monitor.deviceId = P.mon.deviceId;
    s.monitor.level = P.mon.level;
    s.monitor.mute = P.mon.mute;
    if (P.mon.stream) {
        const auto ms = P.mon.stream->status();
        s.monitor.state = ms.state == StreamState::Idle ? "connecting" : streamStateName(ms.state);
        s.monitor.message = ms.message;
    } else {
        s.monitor.state = "off";
        s.monitor.message = "Monitoramento desligado.";
    }
    s.monitor.peakDb[0] = P.monPub[0].load();
    s.monitor.peakDb[1] = P.monPub[1].load();
    return s;
}

std::string AudioEngine::snapshotJson(bool full) const {
    Impl& P = *p_;
    std::string o;
    o.reserve(full ? 8192 : 768);
    if (!full) {
        const hb::audio::ProgramLevels pl = P.mixer.programLevels();
        const std::uint64_t xr = P.xruns.load();
        std::lock_guard lock(P.ctl);
        o += "{\"t\":\"audiolv\",\"strips\":[";
        for (int i = 0; i < P.n; ++i) {
            const StripRT& r = *P.rt[static_cast<std::size_t>(i)];
            const StripCtl& c = P.strips[static_cast<std::size_t>(i)];
            if (i) o += ',';
            o += "{\"pk\":";
            dbNum(o, std::max(r.pub[0].load(), r.pub[1].load()));
            o += ",\"hold\":";
            dbNum(o, std::max(r.pub[2].load(), r.pub[3].load()));
            o += ",\"rms\":";
            dbNum(o, std::max(r.pub[4].load(), r.pub[5].load()));
            o += ",\"ls\":";
            dbNum(o, r.pub[6].load());
            o += ",\"gr\":[";
            bool first = true;
            for (const auto& slot : c.chain->slots) {
                if (!first) o += ',';
                first = false;
                num(o, std::round(slot->gr.load() * 10.0f) / 10.0f);
            }
            o += "]}";
        }
        o += "],\"master\":{\"pk\":";
        dbNum(o, std::max(pl.peakDb[0], pl.peakDb[1]));
        o += ",\"hold\":";
        dbNum(o, std::max(pl.holdDb[0], pl.holdDb[1]));
        o += ",\"rms\":";
        dbNum(o, std::max(pl.rmsDb[0], pl.rmsDb[1]));
        o += ",\"lm\":";
        dbNum(o, pl.momentaryLufs);
        o += ",\"ls\":";
        dbNum(o, pl.shortTermLufs);
        o += ",\"tp\":";
        dbNum(o, std::max(pl.truePeakDb[0], pl.truePeakDb[1]));
        o += ",\"gr\":";
        num(o, std::round(-pl.limiterReductionDb * 10.0f) / 10.0f);
        o += "},\"xr\":" + std::to_string(xr) + "}";
        return o;
    }

    const EngineSnapshot s = snapshot();
    o += "{\"t\":\"audio\",\"ver\":" + std::to_string(s.version) + ",\"strips\":[";
    for (const auto& st : s.strips) {
        if (st.index) o += ',';
        o += "{\"i\":" + std::to_string(st.index) + ",\"name\":" + quote(st.name) + ",\"input\":{\"kind\":\"" +
             inputKindName(st.inputKind) + "\",\"id\":" + quote(st.inputId) + ",\"name\":" + quote(st.inputName) +
             ",\"state\":\"" + st.inputState + "\",\"msg\":" + quote(st.inputMessage) + "},\"trimDb\":";
        num(o, st.trimDb);
        o += ",\"faderDb\":";
        dbNum(o, st.faderDb, 100.0f);
        o += std::string(",\"mute\":") + (st.mute ? "true" : "false") + ",\"solo\":" + (st.solo ? "true" : "false") +
             ",\"pan\":";
        num(o, st.pan);
        o += std::string(",\"phase\":") + (st.phase ? "true" : "false") + ",\"mono\":" + (st.mono ? "true" : "false") +
             ",\"delayMs\":";
        num(o, st.delayMs);
        o += std::string(",\"quality\":\"") + qualityName(st.quality) + "\",\"toProgram\":" +
             (st.toProgram ? "true" : "false") + ",\"toMonitor\":" + (st.toMonitor ? "true" : "false") + ",\"fx\":[";
        for (std::size_t k = 0; k < st.effects.size(); ++k) {
            const auto& e = st.effects[k];
            if (k) o += ',';
            o += "{\"type\":" + quote(e.type) + ",\"name\":" + quote(e.name) + ",\"bypass\":" +
                 (e.bypass ? "true" : "false") + ",\"missing\":" + (e.missing ? "true" : "false") + ",\"params\":[";
            for (std::size_t m = 0; m < e.info.size(); ++m) {
                const auto& pi = e.info[m];
                if (m) o += ',';
                o += "{\"id\":" + quote(pi.id) + ",\"label\":" + quote(pi.label) + ",\"unit\":" + quote(pi.unit) +
                     ",\"min\":";
                num(o, pi.min);
                o += ",\"max\":";
                num(o, pi.max);
                o += ",\"def\":";
                num(o, pi.def);
                o += std::string(",\"log\":") + (pi.logarithmic ? "true" : "false") + ",\"choices\":[";
                for (std::size_t q = 0; q < pi.choices.size(); ++q) {
                    if (q) o += ',';
                    o += quote(pi.choices[q]);
                }
                o += "],\"value\":";
                num(o, m < e.values.size() ? e.values[m] : pi.def);
                o += "}";
            }
            o += "]}";
        }
        o += "]}";
    }
    o += "],\"master\":{\"faderDb\":";
    dbNum(o, s.master.faderDb, 100.0f);
    o += std::string(",\"limiter\":") + (s.master.limiter ? "true" : "false") + ",\"ceilingDb\":";
    num(o, s.master.ceilingDb);
    o += "},\"monitor\":{\"deviceId\":" + quote(s.monitor.deviceId) + ",\"level\":";
    num(o, s.monitor.level);
    o += std::string(",\"mute\":") + (s.monitor.mute ? "true" : "false") + ",\"state\":\"" + s.monitor.state +
         "\",\"msg\":" + quote(s.monitor.message) + "},\"devices\":[";
    for (std::size_t i = 0; i < s.devices.size(); ++i) {
        const auto& d = s.devices[i];
        if (i) o += ',';
        o += "{\"id\":" + quote(d.id) + ",\"name\":" + quote(d.name) + ",\"kind\":\"" + deviceKindName(d.kind) +
             "\",\"isDefault\":" + (d.isDefault ? "true" : "false") + "}";
    }
    o += "],\"effects\":[";
    for (std::size_t i = 0; i < s.effects.size(); ++i) {
        const auto& e = s.effects[i];
        if (i) o += ',';
        o += "{\"typeId\":" + quote(e.typeId) + ",\"name\":" + quote(e.name) + ",\"category\":" + quote(e.category) +
             ",\"source\":" + quote(e.source) + "}";
    }
    o += "]}";
    return o;
}

}  // namespace hb::audio_io
