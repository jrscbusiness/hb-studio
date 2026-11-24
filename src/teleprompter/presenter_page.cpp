#include "teleprompter/presenter_page.hpp"

namespace hb::teleprompter {

std::string extractPresenterPage(const std::string& html) {
    static const std::string open = "<template id=\"tpPresenterPage\">";
    static const std::string close = "</template>";
    const auto b = html.find(open);
    if (b == std::string::npos) return {};
    const auto start = b + open.size();
    const auto e = html.find(close, start);
    if (e == std::string::npos) return {};
    return html.substr(start, e - start);
}

}  // namespace hb::teleprompter
