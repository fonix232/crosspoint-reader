#pragma once
#include <Epub.h>

#include <memory>
#include <string>

// Resolve rendered text against the original XHTML; never estimate an XPath
// from rendered character counts (hyphenation/whitespace change those).
bool resolveHighlightText(const std::shared_ptr<Epub>& epub, int spine, uint32_t pageOffset, const std::string& prefix,
                          const std::string& text, uint32_t& start, uint32_t& end);
