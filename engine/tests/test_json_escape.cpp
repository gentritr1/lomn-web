#include <string>

#include "doctest.h"
#include "json_escape.h"

TEST_CASE("json_escape: plain text passes through") { CHECK(lomn::json_escape("Apple M2 (Metal)") == "Apple M2 (Metal)"); }

TEST_CASE("json_escape: quote and backslash") {
  CHECK(lomn::json_escape("a\"b") == "a\\\"b");
  CHECK(lomn::json_escape("a\\b") == "a\\\\b");
}

TEST_CASE("json_escape: control characters") {
  CHECK(lomn::json_escape("a\nb") == "a\\u000ab");
  CHECK(lomn::json_escape(std::string("x\x01y", 3)) == "x\\u0001y");
  CHECK(lomn::json_escape(std::string("\x1f", 1)) == "\\u001f");
  CHECK(lomn::json_escape(" ") == " ");
  CHECK(lomn::json_escape("\x7f") == "\x7f");  // DEL is not a JSON-escaped control character
}

TEST_CASE("json_escape: UTF-8 multibyte passes through unchanged") {
  CHECK(lomn::json_escape("\xc3\xa9") == "\xc3\xa9");
  CHECK(lomn::json_escape("caf\xc3\xa9 \xe2\x82\xac") == "caf\xc3\xa9 \xe2\x82\xac");
}
