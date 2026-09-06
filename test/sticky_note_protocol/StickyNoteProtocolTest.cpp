#include <array>
#include <cassert>
#include <cstring>
#include <iostream>

#include "src/features/sticky_notes/StickyNoteProtocol.h"

namespace {
sticky_note::Note makeNote(const uint16_t year, const uint8_t month, const uint8_t day, const char* message) {
  sticky_note::Note note;
  note.year = year;
  note.month = month;
  note.day = day;
  note.messageLength = static_cast<uint16_t>(std::strlen(message));
  std::memcpy(note.message.data(), message, note.messageLength);
  note.message[note.messageLength] = '\0';
  return note;
}
}  // namespace

int main() {
  std::array<uint8_t, sticky_note::SNAPSHOT_CONTROL_BYTES> packet{};
  assert(sticky_note::encodeSnapshotControl(sticky_note::TYPE_SNAPSHOT_BEGIN, 0x12345678U, 0x1357U,
                                            0x89abcdefU, packet.data()) == sticky_note::SNAPSHOT_CONTROL_BYTES);

  const std::array<uint8_t, sticky_note::SNAPSHOT_CONTROL_BYTES> expected = {
      'C', 'I', 'N', 'T', 3, 3, 0, 0, 0x78, 0x56, 0x34, 0x12, 0x57, 0x13, 0, 0, 0xef, 0xcd, 0xab, 0x89};
  assert(packet == expected);

  sticky_note::SnapshotControl control;
  assert(sticky_note::decodeSnapshotControl(packet.data(), packet.size(), control));
  assert(control.type == sticky_note::TYPE_SNAPSHOT_BEGIN);
  assert(control.sequence == 0x12345678U);
  assert(control.entryCount == 0x1357U);
  assert(control.digest == 0x89abcdefU);

  assert(sticky_note::encodeSnapshotControl(sticky_note::TYPE_SNAPSHOT_COMMIT, 7, 0, 0, packet.data()) ==
         sticky_note::SNAPSHOT_CONTROL_BYTES);
  packet[14] = 1;
  assert(!sticky_note::decodeSnapshotControl(packet.data(), packet.size(), control));
  packet[14] = 0;
  sticky_note::writeU32Le(packet.data() + 8, 0);
  assert(!sticky_note::decodeSnapshotControl(packet.data(), packet.size(), control));

  const sticky_note::Note normalized = makeNote(2026, 9, 6, "first second third");
  const sticky_note::Note controls = makeNote(2026, 9, 6, "first\tsecond\rthird");
  const sticky_note::Note nextDay = makeNote(2026, 9, 7, "first second third");
  const uint32_t normalizedDigest =
      sticky_note::snapshotDigestFinish(sticky_note::snapshotDigestUpdate(0xffffffffU, normalized));
  const uint32_t controlsDigest =
      sticky_note::snapshotDigestFinish(sticky_note::snapshotDigestUpdate(0xffffffffU, controls));
  const uint32_t nextDayDigest =
      sticky_note::snapshotDigestFinish(sticky_note::snapshotDigestUpdate(0xffffffffU, nextDay));
  assert(normalizedDigest == controlsDigest);
  assert(normalizedDigest != nextDayDigest);
  assert(sticky_note::snapshotDigestFinish(0xffffffffU) == 0U);

  std::cout << "PASS: v3 snapshot controls, validation, and digest normalization\n";
}
