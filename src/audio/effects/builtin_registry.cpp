#include "audio/effects/builtin_effects.hpp"

namespace hb::audio {

void registerBuiltinEffects() {
    using Info = EffectRegistry::Info;
    auto& r = EffectRegistry::instance();
    // add() ignores duplicates, so calling this more than once is harmless.
    r.add(Info{"hb.gain", "Ganho (trim)", "Utilitários", "builtin"}, [] { return fx::makeGain(); });
    r.add(Info{"hb.phase", "Inverter polaridade", "Utilitários", "builtin"}, [] { return fx::makePhase(); });
    r.add(Info{"hb.mono", "Mono", "Utilitários", "builtin"}, [] { return fx::makeMono(); });
    r.add(Info{"hb.delay", "Atraso (lip-sync)", "Utilitários", "builtin"}, [] { return fx::makeDelay(); });
    r.add(Info{"hb.highpass", "Filtro passa-altas", "Equalização", "builtin"}, [] { return fx::makeHighpass(); });
    r.add(Info{"hb.lowpass", "Filtro passa-baixas", "Equalização", "builtin"}, [] { return fx::makeLowpass(); });
    r.add(Info{"hb.eq3", "Equalizador de 3 bandas", "Equalização", "builtin"}, [] { return fx::makeEq3(); });
    r.add(Info{"hb.gate", "Gate", "Dinâmica", "builtin"}, [] { return fx::makeGate(); });
    r.add(Info{"hb.compressor", "Compressor", "Dinâmica", "builtin"}, [] { return fx::makeCompressor(); });
    r.add(Info{"hb.limiter", "Limitador", "Dinâmica", "builtin"}, [] { return fx::makeLimiter(); });
    r.add(Info{"hb.deesser", "De-esser", "Dinâmica", "builtin"}, [] { return fx::makeDeesser(); });
}

}  // namespace hb::audio
