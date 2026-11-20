#include <memory>
#include <mutex>

#include "recording/avi_writer.hpp"
#include "recording/flac_writer.hpp"
#include "recording/jpeg_seq_writer.hpp"
#include "recording/media_writer.hpp"
#include "recording/wav_writer.hpp"

namespace hb::rec {

namespace {

RecordingProfile audioProfile(const char* id, const char* label, const char* desc, const char* ext, int bits) {
    RecordingProfile p;
    p.id = id;
    p.label = label;
    p.description = desc;
    p.extension = ext;
    p.hasVideo = false;
    p.hasAudio = true;
    p.kind = "audio";
    p.audioBitsPerSample = bits;
    return p;
}

void registerAll() {
    auto& reg = WriterRegistry::instance();
    const auto wav = [] { return std::unique_ptr<IMediaWriter>(new WavWriter()); };
    const auto flac = [] { return std::unique_ptr<IMediaWriter>(new FlacWriter()); };

    reg.add(audioProfile("wav", "WAV — PCM 24 bits (sem perdas)",
                         "Áudio não comprimido, 24 bits; arquivos grandes, máxima compatibilidade.", "wav", 24),
            wav);
    reg.add(audioProfile("wav-16", "WAV — PCM 16 bits",
                         "Áudio não comprimido, 16 bits (qualidade de CD).", "wav", 16),
            wav);
    reg.add(audioProfile("wav-f32", "WAV — float 32 bits",
                         "Áudio em ponto flutuante de 32 bits, sem conversão nem limitação de nível.", "wav", 32),
            wav);
    reg.add(audioProfile("flac", "FLAC — 24 bits (sem perdas)",
                         "Áudio comprimido sem perdas (cerca de metade do tamanho do WAV), 24 bits.", "flac", 24),
            flac);
    reg.add(audioProfile("flac-16", "FLAC — 16 bits (sem perdas)",
                         "Áudio comprimido sem perdas, 16 bits (qualidade de CD).", "flac", 16),
            flac);

    RecordingProfile avi;
    avi.id = "avi-mjpeg";
    avi.label = "AVI — Motion-JPEG + PCM";
    avi.description =
        "Vídeo Motion-JPEG (todos os quadros são chaves, edição fácil) com áudio PCM 16 bits; "
        "arquivos grandes.";
    avi.extension = "avi";
    avi.hasVideo = true;
    avi.hasAudio = true;
    avi.kind = "video";
    avi.videoQuality = 85;
    avi.audioBitsPerSample = 16;
    reg.add(avi, [] { return std::unique_ptr<IMediaWriter>(new AviMjpegWriter()); });

    RecordingProfile seq;
    seq.id = "jpeg-seq";
    seq.label = "Sequência JPEG + WAV";
    seq.description =
        "Pasta com uma imagem JPEG por quadro (frame_000001.jpg, ...) e o áudio em audio.wav ao lado.";
    seq.extension = "";  // the output is a folder
    seq.hasVideo = true;
    seq.hasAudio = true;
    seq.kind = "video";
    seq.videoQuality = 90;
    seq.audioBitsPerSample = 16;
    reg.add(seq, [] { return std::unique_ptr<IMediaWriter>(new JpegSeqWriter()); });
}

}  // namespace

void registerPortableWriters() {
    static std::once_flag once;  // WriterRegistry::add already rejects duplicates; this just avoids the work
    std::call_once(once, registerAll);
}

}  // namespace hb::rec
