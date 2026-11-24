#pragma once
// Test doubles for the cloud module: scripted HTTP transport, in-memory file system, fake provider.
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cloud/cloud_types.hpp"
#include "cloud/file_system.hpp"

namespace cloudtest {

using namespace hb::cloud;

inline HttpResponse resp(int status, std::string body = "", std::vector<std::pair<std::string, std::string>> headers = {}) {
    HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    r.headers = std::move(headers);
    return r;
}

class FakeHttp : public ICloudHttp {
public:
    std::vector<HttpRequest> reqs;
    std::function<Result<HttpResponse>(const HttpRequest&)> handler;
    Result<HttpResponse> send(const HttpRequest& r) override {
        reqs.push_back(r);
        if (r.gate && !r.body.empty() && !r.gate->acquire(r.body.size())) return makeError(ErrorKind::Cancelled, "abortado");
        if (!handler) return makeError(ErrorKind::Transient, "no handler");
        return handler(r);
    }
};

inline std::string hdr(const HttpRequest& r, const std::string& name) {
    for (auto& h : r.headers)
        if (h.first == name) return h.second;
    return {};
}

class MemFs : public IFileSystem {
public:
    struct F { std::string data; std::int64_t mtime = 0; };
    std::map<std::string, F> files;
    std::set<std::string> locked;
    FileStat stat(const std::string& p) override {
        FileStat s;
        auto it = files.find(p);
        if (it == files.end()) return s;
        s.exists = true;
        s.size = it->second.data.size();
        s.mtime = it->second.mtime;
        return s;
    }
    std::vector<std::string> listFiles(const std::string& dir) override {
        std::vector<std::string> o;
        for (auto& kv : files)
            if (kv.first.rfind(dir + "/", 0) == 0 && kv.first.find('/', dir.size() + 1) == std::string::npos) o.push_back(kv.first);
        return o;
    }
    bool read(const std::string& p, std::uint64_t off, std::size_t len, std::string& out) override {
        auto it = files.find(p);
        if (it == files.end() || off > it->second.data.size()) return false;
        out = it->second.data.substr(static_cast<std::size_t>(off), len);
        return true;
    }
    bool remove(const std::string& p) override { return files.erase(p) > 0; }
    bool isLocked(const std::string& p) override { return locked.count(p) > 0; }
};

// Provider that "stores" objects in memory, with scriptable failures.
class FakeProvider : public ICloudProvider {
public:
    std::uint64_t chunk = 4;
    std::map<std::string, std::string> objects;       // finished objects
    std::map<std::string, std::string> partial;       // in-flight sessions: remoteName -> bytes so far
    std::vector<Error> failNextPut;                    // popped one per putChunk call
    std::vector<Error> failNextBegin;
    int begins = 0, puts = 0, finishes = 0, aborts = 0, commits = 0, stats = 0;
    bool wholeMd5 = false;
    ITransferGate* lastGate = nullptr;
    std::function<void()> onPut;

    const char* id() const override { return "fake"; }
    Result<std::string> test() override { return std::string("ok"); }
    Result<RemoteStat> stat(const FileInfo& f) override {
        ++stats;
        RemoteStat s;
        auto it = objects.find(f.remoteName);
        if (it != objects.end()) { s.exists = true; s.size = it->second.size(); }
        return s;
    }
    Result<UploadSession> begin(const FileInfo& f) override {
        ++begins;
        if (!failNextBegin.empty()) { Error e = failNextBegin.front(); failNextBegin.erase(failNextBegin.begin()); return e; }
        UploadSession s;
        s.id = "sess-" + std::to_string(begins);
        s.chunkSize = chunk;
        partial[f.remoteName].clear();
        return s;
    }
    Result<std::uint64_t> committed(UploadSession&, const FileInfo& f) override {
        ++commits;
        auto it = partial.find(f.remoteName);
        if (it == partial.end()) return makeError(ErrorKind::SessionLost, "gone");
        return static_cast<std::uint64_t>(it->second.size());
    }
    Result<ChunkAck> putChunk(UploadSession&, const FileInfo& f, std::uint64_t offset, std::string_view data, ITransferGate* gate) override {
        ++puts;
        lastGate = gate;
        if (onPut) onPut();
        if (!failNextPut.empty()) { Error e = failNextPut.front(); failNextPut.erase(failNextPut.begin()); return e; }
        if (gate && !gate->acquire(data.size())) return makeError(ErrorKind::Cancelled, "abortado");
        auto& p = partial[f.remoteName];
        if (p.size() != offset) return makeError(ErrorKind::Permanent, "offset errado");
        p.append(data);
        ChunkAck a;
        a.offset = offset;
        a.size = a.accepted = data.size();
        return a;
    }
    Result<RemoteStat> finish(UploadSession&, const FileInfo& f, const std::string&) override {
        ++finishes;
        objects[f.remoteName] = partial[f.remoteName];
        partial.erase(f.remoteName);
        RemoteStat s;
        s.exists = true;
        s.size = f.size;
        return s;
    }
    bool verifiesWholeFileMd5() const override { return wholeMd5; }
    void abort(const UploadSession&, const FileInfo& f) noexcept override { ++aborts; partial.erase(f.remoteName); }
};

}  // namespace cloudtest
