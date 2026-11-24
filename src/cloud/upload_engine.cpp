#include "cloud/upload_engine.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cloud/hash_util.hpp"
#include "cloud/sigv4.hpp"
#include "core/json.hpp"

namespace hb::cloud {

using Json = hb::core::json::Value;

const char* jobStateName(JobState s) {
    switch (s) {
        case JobState::Queued: return "queued";
        case JobState::Uploading: return "uploading";
        case JobState::Done: return "done";
        case JobState::Error: return "error";
        case JobState::Cancelled: return "cancelled";
    }
    return "error";
}

namespace {

Json jS(const std::string& s) { Json v; v.t = Json::T::Str; v.s = s; return v; }
Json jN(double n) { Json v; v.t = Json::T::Num; v.n = n; return v; }
Json jB(bool b) { Json v; v.t = Json::T::Bool; v.b = b; return v; }
Json jO() { Json v; v.t = Json::T::Obj; return v; }
Json jA() { Json v; v.t = Json::T::Arr; return v; }

std::string gS(const Json& o, const char* k) { auto* v = o.get(k); return v && v->isStr() ? v->s : std::string(); }
double gN(const Json& o, const char* k) { auto* v = o.get(k); return v && v->isNum() ? v->n : 0; }
bool gB(const Json& o, const char* k) { auto* v = o.get(k); return v && v->isBool() && v->b; }
// Sizes/offsets are stored as decimal strings: JSON doubles are exact only up to 2^53.
std::uint64_t gU(const Json& o, const char* k) { return std::strtoull(gS(o, k).c_str(), nullptr, 10); }
Json jU(std::uint64_t u) { return jS(std::to_string(u)); }

Json ackToJson(const ChunkAck& a) {
    Json o = jO();
    o.o["i"] = jN(a.index);
    o.o["off"] = jU(a.offset);
    o.o["size"] = jU(a.size);
    o.o["etag"] = jS(a.etag);
    o.o["md5"] = jS(a.md5);
    o.o["sha"] = jS(a.sha256);
    return o;
}
ChunkAck ackFromJson(const Json& o) {
    ChunkAck a;
    a.index = static_cast<std::uint32_t>(gN(o, "i"));
    a.offset = gU(o, "off");
    a.size = gU(o, "size");
    a.accepted = a.size;
    a.etag = gS(o, "etag");
    a.md5 = gS(o, "md5");
    a.sha256 = gS(o, "sha");
    return a;
}

Json jobToJson(const Job& j) {
    Json o = jO();
    o.o["id"] = jS(j.id);
    o.o["path"] = jS(j.path);
    o.o["kind"] = jS(j.kind);
    o.o["index"] = jN(j.index);
    o.o["remote"] = jS(j.remoteName);
    o.o["size"] = jU(j.size);
    o.o["mtime"] = jN(static_cast<double>(j.mtime));
    o.o["closed"] = jB(j.closedByProducer);
    o.o["state"] = jS(jobStateName(j.state));
    o.o["attempts"] = jN(j.attempts);
    o.o["nextAt"] = jN(static_cast<double>(j.nextAt));
    o.o["done"] = jU(j.done);
    o.o["error"] = jS(j.error);
    o.o["errorKind"] = jS(j.errorKind);
    o.o["note"] = jS(j.note);
    o.o["createdAt"] = jN(static_cast<double>(j.createdAt));
    o.o["finishedAt"] = jN(static_cast<double>(j.finishedAt));
    o.o["remoteId"] = jS(j.remoteId);
    if (j.hasSession) {
        Json s = jO();
        s.o["id"] = jS(j.session.id);
        s.o["uri"] = jS(j.session.uri);
        s.o["chunk"] = jU(j.session.chunkSize);
        Json acks = jA();
        for (const auto& a : j.session.acks) acks.a.push_back(ackToJson(a));
        s.o["acks"] = std::move(acks);
        o.o["session"] = std::move(s);
    }
    return o;
}

bool jobFromJson(const Json& o, Job& j) {
    if (!o.isObj()) return false;
    j.id = gS(o, "id");
    j.path = gS(o, "path");
    if (j.id.empty() || j.path.empty()) return false;
    j.kind = gS(o, "kind");
    j.index = static_cast<int>(gN(o, "index"));
    j.remoteName = gS(o, "remote");
    j.size = gU(o, "size");
    j.mtime = static_cast<std::int64_t>(gN(o, "mtime"));
    j.closedByProducer = gB(o, "closed");
    const std::string st = gS(o, "state");
    j.state = st == "done" ? JobState::Done : st == "error" ? JobState::Error : st == "cancelled" ? JobState::Cancelled : JobState::Queued;
    j.attempts = static_cast<int>(gN(o, "attempts"));
    j.nextAt = static_cast<std::int64_t>(gN(o, "nextAt"));
    j.done = gU(o, "done");
    j.error = gS(o, "error");
    j.errorKind = gS(o, "errorKind");
    j.note = gS(o, "note");
    j.createdAt = static_cast<std::int64_t>(gN(o, "createdAt"));
    j.finishedAt = static_cast<std::int64_t>(gN(o, "finishedAt"));
    j.remoteId = gS(o, "remoteId");
    if (const Json* s = o.get("session"); s && s->isObj()) {
        j.hasSession = true;
        j.session.id = gS(*s, "id");
        j.session.uri = gS(*s, "uri");
        j.session.chunkSize = gU(*s, "chunk");
        if (const Json* acks = s->get("acks"); acks && acks->isArr())
            for (const auto& a : acks->a) j.session.acks.push_back(ackFromJson(a));
        if (j.session.chunkSize == 0) j.hasSession = false;
    }
    return true;
}

std::string contentTypeFor(const std::string& path) {
    const std::string e = fileExtension(path);
    if (e == "mp4") return "video/mp4";
    if (e == "mkv") return "video/x-matroska";
    if (e == "mov") return "video/quicktime";
    if (e == "avi") return "video/x-msvideo";
    if (e == "wav") return "audio/wav";
    if (e == "flac") return "audio/flac";
    return "application/octet-stream";
}

std::string safeBase(const std::string& path) {
    std::string b = baseName(path), o;
    for (unsigned char c : b) o += (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') ? '_' : static_cast<char>(c);
    return o.empty() ? "arquivo" : o;
}

}  // namespace

// Throttles and polices the transport: called before each slice of body bytes goes on the wire.
class UploadEngine::Gate : public ITransferGate {
public:
    explicit Gate(UploadEngine& e) : e_(e) {}
    bool acquire(std::size_t bytes) override {
        for (;;) {
            if (e_.abortRequested()) return false;
            const double wait = e_.bucket_.tryTake(static_cast<double>(bytes));
            if (wait <= 0) break;
            e_.o_.sleepFn(std::clamp(wait, 0.002, 0.1));  // >= 2 ms: tiny waits would not advance a float clock
        }
        e_.inflight_ += bytes;
        e_.sentTotal_ += bytes;
        return true;
    }

private:
    UploadEngine& e_;
};

UploadEngine::UploadEngine(EngineOptions opts) : o_(std::move(opts)), bucket_([this] { return o_.monoNow(); }) {
    if (!o_.fs) o_.fs = makeStdFileSystem();
    if (!o_.wallNow)
        o_.wallNow = [] { return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()); };
    if (!o_.monoNow)
        o_.monoNow = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    if (!o_.sleepFn) o_.sleepFn = [](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); };
    bucket_.setRate(0);
}

std::int64_t UploadEngine::now() const { return o_.wallNow(); }

void UploadEngine::setProvider(std::shared_ptr<ICloudProvider> p) {
    {
        std::lock_guard lock(mu_);
        provider_ = std::move(p);
        touch();
    }
    wake();
}

void UploadEngine::setBandwidthMbps(double mbps) { bucket_.setRate(mbps > 0 ? mbpsToBytesPerSec(mbps) : 0); }

void UploadEngine::setEnabled(bool on) {
    enabled_ = on;
    touch();
    if (on) wake();
}

std::string UploadEngine::remoteNameFor(const std::string& path, const std::string& kind, std::int64_t mtime) {
    const std::string d = formatAmzDate(std::chrono::system_clock::time_point(std::chrono::seconds(mtime))).substr(0, 8);
    const std::string date = d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
    std::string k = kind;
    if (k != "program" && k != "iso-video" && k != "iso-audio") k = "outros";
    return date + "/" + k + "/" + safeBase(path);
}

double UploadEngine::backoffFor(int attempts) const {
    double d = o_.backoffBaseSec;
    for (int i = 1; i < attempts && d < o_.backoffCapSec; ++i) d *= 2;
    return std::min(d, o_.backoffCapSec);
}

UploadEngine::Enqueue UploadEngine::enqueue(const std::string& path, const std::string& kind, int index, bool closed, std::string* why) {
    auto reject = [&](const char* m) {
        if (why) *why = m;
        return Enqueue::Rejected;
    };
    const std::string ext = fileExtension(path);
    if (std::find(o_.extensions.begin(), o_.extensions.end(), ext) == o_.extensions.end()) return reject("extensão não permitida");
    const FileStat st = o_.fs->stat(path);
    if (!st.exists || st.isDir) return reject("arquivo não encontrado");
    if (st.size == 0) return reject("arquivo vazio");
    {
        std::lock_guard lock(mu_);
        for (auto& j : jobs_) {
            if (j.path != path) continue;
            if (j.size == st.size && j.mtime == st.mtime) {
                if (closed && !j.closedByProducer && j.state != JobState::Done) { j.closedByProducer = true; touch(); }
                return Enqueue::Duplicate;
            }
            if (j.state == JobState::Queued || j.state == JobState::Error) {  // same file, grew/changed: refresh
                j.size = st.size;
                j.mtime = st.mtime;
                j.closedByProducer = closed;
                j.state = JobState::Queued;
                j.hasSession = false;
                j.session = {};
                j.done = 0;
                j.attempts = 0;
                j.error.clear();
                j.remoteName = remoteNameFor(path, kind, st.mtime);
                touch();
                saveLocked();
                wake();
                return Enqueue::Added;
            }
        }
        Job j;
        j.id = "u" + std::to_string(now()) + "-" + std::to_string(nextId_++);
        j.path = path;
        j.kind = kind;
        j.index = index;
        j.size = st.size;
        j.mtime = st.mtime;
        j.closedByProducer = closed;
        j.remoteName = remoteNameFor(path, kind, st.mtime);
        j.createdAt = now();
        j.nextAt = closed ? 0 : now() + o_.stableSeconds;
        jobs_.push_back(std::move(j));
        pruneLocked();
        touch();
        saveLocked();
    }
    wake();
    return Enqueue::Added;
}

void UploadEngine::pruneLocked() {
    while (jobs_.size() > o_.maxJobs) {
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.state == JobState::Done || j.state == JobState::Cancelled; });
        if (it == jobs_.end()) break;
        jobs_.erase(it);
    }
}

bool UploadEngine::retry(const std::string& id) {
    std::lock_guard lock(mu_);
    for (auto& j : jobs_) {
        if (j.id != id || (j.state != JobState::Error && j.state != JobState::Cancelled)) continue;
        j.state = JobState::Queued;
        j.attempts = 0;
        j.nextAt = 0;
        j.closedByProducer = true;  // the user asked for it explicitly
        j.error.clear();
        j.errorKind.clear();
        touch();
        saveLocked();
        wake();
        return true;
    }
    return false;
}

int UploadEngine::retryAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(mu_);
        for (auto& j : jobs_)
            if (j.state == JobState::Error) ids.push_back(j.id);
    }
    int n = 0;
    for (const auto& id : ids) n += retry(id) ? 1 : 0;
    return n;
}

bool UploadEngine::cancel(const std::string& id) {
    std::shared_ptr<ICloudProvider> prov;
    Job copy;
    bool hadSession = false;
    {
        std::lock_guard lock(mu_);
        for (auto& j : jobs_) {
            if (j.id != id || j.state == JobState::Done || j.state == JobState::Cancelled) continue;
            if (id == currentId_) cancelCurrent_ = true;
            hadSession = j.hasSession && id != currentId_;
            copy = j;
            j.state = JobState::Cancelled;
            j.error.clear();
            j.hasSession = false;
            j.session = {};
            j.finishedAt = now();
            prov = provider_;
            touch();
            saveLocked();
            goto found;
        }
        return false;
    }
found:
    if (hadSession && prov) {  // outside the lock: network
        FileInfo fi{copy.path, copy.remoteName, copy.size, contentTypeFor(copy.path)};
        prov->abort(copy.session, fi);
    }
    return true;
}

int UploadEngine::clearFinished() {
    std::lock_guard lock(mu_);
    const auto before = jobs_.size();
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.state == JobState::Done || j.state == JobState::Cancelled; }), jobs_.end());
    if (jobs_.size() != before) { touch(); saveLocked(); }
    return static_cast<int>(before - jobs_.size());
}

bool UploadEngine::paused(std::string* reason) const {
    if (!enabled_) { if (reason) *reason = "Backup desligado"; return true; }
    if (pauseWhileStreaming_ && o_.streamingActive && o_.streamingActive()) {
        if (reason) *reason = "Pausado durante a transmissão";
        return true;
    }
    return false;
}

bool UploadEngine::abortRequested() const { return stopping_ || cancelCurrent_ || paused(nullptr); }

std::vector<Job> UploadEngine::jobs() const {
    std::vector<Job> out;
    std::lock_guard lock(mu_);
    out = jobs_;
    return out;
}

Totals UploadEngine::totals() {
    Totals t;
    {
        std::lock_guard lock(mu_);
        for (const auto& j : jobs_) {
            switch (j.state) {
                case JobState::Queued: ++t.queued; break;
                case JobState::Uploading: ++t.uploading; break;
                case JobState::Done: ++t.done; break;
                case JobState::Error: ++t.error; break;
                case JobState::Cancelled: ++t.cancelled; continue;
            }
            t.bytesTotal += j.size;
            t.bytesDone += j.state == JobState::Done ? j.size : j.done + (j.id == currentId_ ? inflight_.load() : 0);
        }
    }
    t.paused = paused(&t.pauseReason);
    const double m = o_.monoNow();
    std::lock_guard lock(rateMu_);
    rate_.emplace_back(m, sentTotal_.load());
    while (rate_.size() > 2 && m - rate_.front().first > 6.0) rate_.pop_front();
    if (rate_.size() >= 2 && rate_.back().first > rate_.front().first)
        t.bytesPerSec = static_cast<double>(rate_.back().second - rate_.front().second) / (rate_.back().first - rate_.front().first);
    return t;
}

void UploadEngine::fail(Job& j, const Error& e, const std::shared_ptr<ICloudProvider>& prov) {
    j.errorKind = errorKindName(e.kind);
    switch (e.kind) {
        case ErrorKind::Cancelled:
            j.state = JobState::Queued;  // not a failure; resumes when allowed
            j.error.clear();
            j.errorKind.clear();
            return;
        case ErrorKind::SessionLost:
            if (j.hasSession && prov) {
                FileInfo fi{j.path, j.remoteName, j.size, contentTypeFor(j.path)};
                prov->abort(j.session, fi);
            }
            j.hasSession = false;
            j.session = {};
            j.done = 0;
            [[fallthrough]];
        case ErrorKind::Transient:
            j.error = e.message;
            ++j.attempts;
            if (j.attempts >= o_.maxAttempts) {
                j.state = JobState::Error;
                j.error = "Falhou após " + std::to_string(j.attempts) + " tentativas: " + e.message;
            } else {
                j.state = JobState::Queued;
                j.nextAt = now() + static_cast<std::int64_t>(e.kind == ErrorKind::SessionLost ? 1.0 : backoffFor(j.attempts));
            }
            return;
        default:
            j.state = JobState::Error;
            j.error = e.message;
            j.finishedAt = now();
            return;
    }
}

bool UploadEngine::md5OfFile(const std::string& path, std::uint64_t size, std::string& out) {
    Md5 m;
    std::string buf;
    const std::size_t slice = 4 * 1024 * 1024;
    for (std::uint64_t off = 0; off < size; off += slice) {
        if (abortRequested()) return false;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(slice, size - off));
        if (!o_.fs->read(path, off, n, buf) || buf.size() != n) return false;
        m.update(buf.data(), buf.size());
    }
    out = m.finishHex();
    return true;
}

void UploadEngine::finalize(Job& j, const std::shared_ptr<ICloudProvider>& prov, const FileInfo& fi) {
    std::string md5;
    if (prov->verifiesWholeFileMd5() && !md5OfFile(j.path, j.size, md5)) {
        if (abortRequested()) return fail(j, makeError(ErrorKind::Cancelled, ""), prov);
        return fail(j, makeError(ErrorKind::Local, "Não foi possível reler o arquivo para conferir o MD5"), prov);
    }
    auto r = prov->finish(j.session, fi, md5);
    if (!r.ok()) return fail(j, r.error(), prov);
    j.state = JobState::Done;
    j.done = j.size;
    j.error.clear();
    j.errorKind.clear();
    j.attempts = 0;
    j.remoteId = r.value().remoteId;
    if (!r.value().note.empty()) j.note = r.value().note;
    j.finishedAt = now();
    j.hasSession = false;
    j.session = {};
    // Delete-after-upload only after the provider confirmed completion and the local file is unchanged.
    if (deleteAfter_) {
        const FileStat st = o_.fs->stat(j.path);
        if (st.exists && st.size == j.size && st.mtime == j.mtime) {
            if (o_.fs->remove(j.path)) j.note += (j.note.empty() ? "" : " ") + std::string("Arquivo local apagado após o envio.");
            else j.note += (j.note.empty() ? "" : " ") + std::string("Não foi possível apagar o arquivo local.");
        }
    }
}

void UploadEngine::process(Job& j, const std::shared_ptr<ICloudProvider>& prov) {
    const FileStat st = o_.fs->stat(j.path);
    if (!st.exists || st.isDir) return fail(j, makeError(ErrorKind::Local, "Arquivo local não encontrado (movido ou apagado?)"), prov);
    if (st.size != j.size || st.mtime != j.mtime) {  // the file changed since it was queued
        if (j.hasSession) {
            FileInfo old{j.path, j.remoteName, j.size, contentTypeFor(j.path)};
            prov->abort(j.session, old);
        }
        j.hasSession = false;
        j.session = {};
        j.done = 0;
        j.size = st.size;
        j.mtime = st.mtime;
        if (!j.closedByProducer) {
            j.state = JobState::Queued;
            j.nextAt = now() + o_.stableSeconds;
            j.error = "Arquivo ainda mudando: aguardando estabilizar";
            return;
        }
    }
    if (st.size == 0) return fail(j, makeError(ErrorKind::Local, "Arquivo vazio"), prov);
    if (!j.closedByProducer) {  // never upload something that may still be being written
        if (now() - st.mtime < o_.stableSeconds || o_.fs->isLocked(j.path)) {
            j.state = JobState::Queued;
            j.nextAt = now() + std::max(5, o_.stableSeconds / 3);
            j.error = "Aguardando o arquivo ser fechado";
            return;
        }
    }
    const FileInfo fi{j.path, j.remoteName, j.size, contentTypeFor(j.path)};
    Gate gate(*this);

    if (!j.hasSession) {
        if (!dedupeDone_.count(j.id)) {
            auto rs = prov->stat(fi);
            if (!rs.ok()) return fail(j, rs.error(), prov);
            dedupeDone_.insert(j.id);
            if (rs.value().exists && rs.value().size == j.size) {
                j.state = JobState::Done;
                j.done = j.size;
                j.error.clear();
                j.note = "Já existia no destino com o mesmo tamanho: não reenviado.";
                j.remoteId = rs.value().remoteId;
                j.finishedAt = now();
                return;
            }
        }
        auto bs = prov->begin(fi);
        if (!bs.ok()) return fail(j, bs.error(), prov);
        j.session = bs.value();
        j.hasSession = true;
        j.done = 0;
        resync_.erase(j.id);
    } else if (resync_.count(j.id)) {
        auto c = prov->committed(j.session, fi);
        if (!c.ok()) return fail(j, c.error(), prov);
        j.done = std::min<std::uint64_t>(c.value(), j.size);
        resync_.erase(j.id);
    }

    if (j.done >= j.size) return finalize(j, prov, fi);

    const std::uint64_t len = std::min<std::uint64_t>(j.session.chunkSize, j.size - j.done);
    std::string data;
    if (!o_.fs->read(j.path, j.done, static_cast<std::size_t>(len), data) || data.size() != len)
        return fail(j, makeError(ErrorKind::Local, "Falha ao ler o arquivo local (em uso, encolheu ou ilegível)"), prov);
    inflight_ = 0;
    auto ack = prov->putChunk(j.session, fi, j.done, data, &gate);
    inflight_ = 0;
    if (!ack.ok()) return fail(j, ack.error(), prov);
    j.done += ack.value().accepted;
    j.attempts = 0;
    j.error.clear();
    j.errorKind.clear();
    j.state = JobState::Uploading;
    j.nextAt = 0;
    if (j.done >= j.size) finalize(j, prov, fi);
}

bool UploadEngine::runOnce() {
    if (paused(nullptr)) return false;
    std::shared_ptr<ICloudProvider> prov;
    Job j;
    {
        std::lock_guard lock(mu_);
        prov = provider_;
        if (!prov) return false;
        const std::int64_t t = now();
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& x) {
            return (x.state == JobState::Queued || x.state == JobState::Uploading) && x.nextAt <= t;
        });
        if (it == jobs_.end()) return false;
        it->state = JobState::Uploading;
        currentId_ = it->id;
        cancelCurrent_ = false;
        j = *it;
    }
    process(j, prov);
    bool cancelled = false;
    {
        std::lock_guard lock(mu_);
        for (auto& x : jobs_) {
            if (x.id != j.id) continue;
            if (x.state == JobState::Cancelled) cancelled = true;
            else x = j;
            break;
        }
        currentId_.clear();
        cancelCurrent_ = false;
        touch();
        saveLocked();
    }
    if (cancelled && j.hasSession) {
        FileInfo fi{j.path, j.remoteName, j.size, contentTypeFor(j.path)};
        prov->abort(j.session, fi);
    }
    return true;
}

void UploadEngine::saveLocked() {
    if (o_.journalPath.empty()) return;
    Json root = jO();
    root.o["schema"] = jN(1);
    root.o["nextId"] = jN(static_cast<double>(nextId_));
    Json arr = jA();
    for (const auto& j : jobs_) arr.a.push_back(jobToJson(j));
    root.o["jobs"] = std::move(arr);
    const std::string text = hb::core::json::dump(root, true);
    const std::string tmp = o_.journalPath + ".tmp";
    {
        std::ofstream f(std::filesystem::path(std::u8string(tmp.begin(), tmp.end())), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << text;
        if (!f.good()) return;
    }
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(std::u8string(tmp.begin(), tmp.end())),
                            std::filesystem::path(std::u8string(o_.journalPath.begin(), o_.journalPath.end())), ec);
}

bool UploadEngine::saveJournal() {
    std::lock_guard lock(mu_);
    saveLocked();
    return !o_.journalPath.empty();
}

bool UploadEngine::loadJournal() {
    if (o_.journalPath.empty()) return false;
    std::string text;
    {
        std::ifstream f(std::filesystem::path(std::u8string(o_.journalPath.begin(), o_.journalPath.end())), std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    Json root;
    hb::core::json::Limits lim;
    lim.maxBytes = 16 * 1024 * 1024;
    lim.maxArray = 20000;
    if (!hb::core::json::parse(text, root, lim) || !root.isObj()) return false;
    const Json* arr = root.get("jobs");
    if (!arr || !arr->isArr()) return false;
    std::lock_guard lock(mu_);
    jobs_.clear();
    resync_.clear();
    for (const auto& jv : arr->a) {
        Job j;
        if (!jobFromJson(jv, j)) continue;
        if (j.state == JobState::Uploading) j.state = JobState::Queued;  // interrupted by a restart
        if (j.state == JobState::Queued && j.hasSession) resync_.insert(j.id);
        jobs_.push_back(std::move(j));
        if (jobs_.size() >= o_.maxJobs * 2) break;
    }
    nextId_ = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(gN(root, "nextId")));
    touch();
    return true;
}

void UploadEngine::start() {
    if (worker_.joinable()) return;
    stopping_ = false;
    worker_ = std::thread([this] { workerLoop(); });
}

void UploadEngine::stop() {
    stopping_ = true;
    wake();
    if (worker_.joinable()) worker_.join();
}

void UploadEngine::wake() {
    std::lock_guard lock(cvMu_);
    cv_.notify_all();
}

void UploadEngine::workerLoop() {
    try {
        if (o_.threadInit) o_.threadInit();
    } catch (...) {
    }
    while (!stopping_) {
        bool worked = false;
        try {
            worked = runOnce();
        } catch (...) {  // never let an unexpected exception kill the worker
            worked = false;
        }
        if (!worked) {
            std::unique_lock lock(cvMu_);
            cv_.wait_for(lock, std::chrono::seconds(2));
        }
    }
}

}  // namespace hb::cloud
