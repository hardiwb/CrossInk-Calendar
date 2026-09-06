add_executable(StickyNoteProtocolTest
  StickyNoteProtocolTest.cpp
)

target_link_libraries(StickyNoteProtocolTest PRIVATE
  crosspoint_test_common
)

add_test(NAME StickyNoteProtocolTest COMMAND StickyNoteProtocolTest)
