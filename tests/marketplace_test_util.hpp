#pragma once
#include <string>

namespace hb::mkttest {

inline const std::string kHash(64, 'a');

// A valid theme entry; tests tweak it with replace().
inline std::string themeEntry(const std::string& id = "com.example.tema", const std::string& sha = kHash, unsigned long long size = 800,
                              const std::string& version = "1.0.0", const std::string& url = "https://example.com/tema.hbplugin") {
    return R"({"id":")" + id + R"(","name":"Tema","author":"Fulano","description":{"pt-BR":"Um tema","en":"A theme"},)" +
           R"("category":"theme","type":"theme","tags":["escuro"],"license":"MIT","verified":false,"permissions":[],)" +
           R"("versions":[{"version":")" + version + R"(","minAppVersion":"0.1.0","platforms":["windows-x64"],"downloadUrls":[")" + url +
           R"("],"sha256":")" + sha + R"(","size":)" + std::to_string(size) + R"(}]})";
}

inline std::string nativeEntry(const std::string& id = "com.example.fonte", const std::string& sha = kHash, unsigned long long size = 5000) {
    return R"({"id":")" + id + R"(","name":"Fonte","author":"Beltrano","description":{"en":"A source"},"category":"video-source","type":"source",)" +
           R"("license":"MIT","permissions":["native-code"],"versions":[{"version":"2.1.0","platforms":["windows-x64"],)" +
           R"("downloadUrls":["https://example.com/f.hbplugin"],"sha256":")" + sha + R"(","size":)" + std::to_string(size) + R"(}]})";
}

inline std::string catalog(const std::string& entries, int schema = 1) {
    return R"({"schemaVersion":)" + std::to_string(schema) + R"(,"plugins":[)" + entries + "]}";
}

inline std::string replaceOnce(std::string s, const std::string& from, const std::string& to) {
    const auto p = s.find(from);
    if (p != std::string::npos) s.replace(p, from.size(), to);
    return s;
}

}  // namespace hb::mkttest
