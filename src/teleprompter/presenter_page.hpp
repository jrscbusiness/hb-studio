#pragma once
#include <string>

namespace hb::teleprompter {

// The presenter window's page lives inside ui/index.html (the UI stays a single embedded file, no
// extra resource to ship) as the inert content of <template id="tpPresenterPage">...</template>.
// Returns that content (a complete HTML document), or "" if the template is missing/unterminated.
// The presenter page must therefore never contain the literal text "</template>".
std::string extractPresenterPage(const std::string& indexHtml);

}  // namespace hb::teleprompter
