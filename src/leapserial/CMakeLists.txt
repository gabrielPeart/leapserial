set(LeapSerial_SRCS
  Allocation.h
  Allocation.cpp
  Archive.h
  Archive.cpp
  ArchiveFlatbuffer.h
  ArchiveFlatbuffer.cpp
  ArchiveJSON.h
  ArchiveJSON.cpp
  base.h
  Descriptor.h
  Descriptor.cpp
  field_descriptor.h
  field_serializer.h
  field_serializer_t.h
  ProtobufType.h
  IArchiveImpl.h
  IArchiveImpl.cpp
  OArchiveImpl.h
  OArchiveImpl.cpp
  LeapSerial.h
  serial_traits.h
)

rewrite_header_paths(LeapSerial_SRCS)
add_pch(LeapSerial_SRCS "stdafx.h" "stdafx.cpp")
add_library(LeapSerial ${LeapSerial_SRCS})
add_subdirectory(test)

install(TARGETS LeapSerial EXPORT LeapSerialTargets
  DESTINATION lib
  COMPONENT leapserial
  CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES}
)