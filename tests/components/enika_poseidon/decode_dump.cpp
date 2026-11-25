// Reads one hex frame per line from stdin (<length> <data>, no CRC) and prints what poseidon_packet.h makes of it.
//   g++ -std=c++20 -I../../../components/enika_poseidon decode_dump.cpp -o decode_dump
#include "poseidon_packet.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace esphome::enika_poseidon;

static std::string hex(std::span<const uint8_t> v) {
  std::string s;
  char b[3];
  for (auto c : v) {
    snprintf(b, sizeof(b), "%02x", c);
    s += b;
  }
  return s;
}

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::vector<uint8_t> f;
    std::istringstream in(line);
    std::string tok;
    while (in >> tok)
      f.push_back(std::stoi(tok, nullptr, 16));
    StateMessage m;
    if (decode_state_message(f, m)) {
      printf("state id=%06x flags=%02x cnt3=%u init=%d trailer=%u route=%s msg_type=%u", (unsigned) m.id, m.flags,
             m.cnt3, m.init, m.trailer_format, hex(m.route()).c_str(), m.msg_type);
      if (m.has_counter)
        printf(" counter=%u hash=%04x", m.counter, m.hash);
      printf(" payload=%s", hex(m.payload()).c_str());
    } else {
      printf("other kind=%d", static_cast<int>(classify(f)));
    }
    char summary[SUMMARY_SIZE];
    summarize(f, summary);
    printf(" | %s\n", summary);
  }
}
