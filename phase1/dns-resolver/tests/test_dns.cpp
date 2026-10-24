#include <cstdint>
#include <string>
#include <vector>

#include "dns.hpp"
#include "netlab/check.hpp"

namespace {

void test_build_query() {
  auto q = dns::build_query(0xBEEF, "www.example.com", dns::type::A);
  const std::vector<uint8_t> expected = {
      0xBE, 0xEF, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // header, RD set
      3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
      0x00, 0x01, 0x00, 0x01};  // QTYPE A, QCLASS IN
  CHECK(q == expected);

  // Trailing dot means the same name.
  CHECK(dns::build_query(1, "example.com.", dns::type::A) == dns::build_query(1, "example.com", dns::type::A));
  CHECK_THROWS(dns::build_query(1, std::string(64, 'a') + ".com", dns::type::A));
  CHECK_THROWS(dns::build_query(1, "a..b", dns::type::A));
}

// A response for example.com A whose answer name is a compression pointer
// (0xC00C -> offset 12, the question name).
std::vector<uint8_t> sample_response() {
  return {0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
          7,    'e',  'x',  'a',  'm',  'p',  'l',  'e',  3,    'c',  'o',  'm',  0, 0x00, 0x01, 0x00, 0x01,
          // answer 1: A 93.184.216.34, ttl 300
          0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x04, 93, 184, 216, 34,
          // answer 2: MX 10 mail.<ptr to example.com>
          0xC0, 0x0C, 0x00, 0x0F, 0x00, 0x01, 0x00, 0x00, 0x0E, 0x10, 0x00, 0x09, 0x00, 0x0A,
          4,    'm',  'a',  'i',  'l',  0xC0, 0x0C};
}

void test_parse_response() {
  auto buf = sample_response();
  dns::Message m = dns::parse(buf);
  CHECK_EQ(m.header.id, 0x1234);
  CHECK(m.header.is_response());
  CHECK(m.header.recursion_available());
  CHECK_EQ(int(m.header.rcode()), int(dns::rcode::NOERROR));
  CHECK_EQ(m.questions.size(), 1u);
  CHECK_EQ(m.questions[0].name, std::string("example.com."));
  CHECK_EQ(m.answers.size(), 2u);
  CHECK_EQ(m.answers[0].name, std::string("example.com."));
  CHECK_EQ(m.answers[0].ttl, 300u);
  CHECK_EQ(m.answers[0].data, std::string("93.184.216.34"));
  CHECK_EQ(m.answers[1].data, std::string("10 mail.example.com."));
  CHECK_EQ(m.answers[1].target, std::string("mail.example.com."));
}

void test_rejects_malformed() {
  auto buf = sample_response();
  for (size_t cut = 0; cut < buf.size(); ++cut) {
    std::vector<uint8_t> shorter(buf.begin(), buf.begin() + static_cast<long>(cut));
    CHECK_THROWS(dns::parse(shorter));  // every truncation must throw, never read out of bounds
  }

  // A pointer that points at itself.
  std::vector<uint8_t> loop = {0, 1, 0x81, 0x80, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1};
  CHECK_THROWS(dns::parse(loop));

  // Reserved label type 0x40.
  std::vector<uint8_t> reserved = {0, 1, 0x81, 0x80, 0, 1, 0, 0, 0, 0, 0, 0, 0x40, 0, 1, 0, 1};
  CHECK_THROWS(dns::parse(reserved));
}

void test_helpers() {
  CHECK_EQ(dns::canonical("Example.COM"), std::string("example.com."));
  CHECK(dns::type_from_string("aaaa") == dns::type::AAAA);
  CHECK(!dns::type_from_string("bogus"));
  CHECK_EQ(dns::type_to_string(999), std::string("TYPE999"));
  CHECK_EQ(dns::rcode_to_string(dns::rcode::NXDOMAIN), std::string("NXDOMAIN"));
}

}  // namespace

int main() {
  test_build_query();
  test_parse_response();
  test_rejects_malformed();
  test_helpers();
  return netlab::test::finish();
}
