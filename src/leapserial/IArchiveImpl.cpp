// Copyright (C) 2012-2015 Leap Motion, Inc. All rights reserved.
#include "stdafx.h"
#include "IArchiveImpl.h"
#include "field_serializer.h"
#include "serial_traits.h"
#include "ProtobufType.h"
#include "Utility.hpp"
#include <iostream>
#include <sstream>

using namespace leap;

IArchiveImpl::IArchiveImpl(IInputStream& is) :
  is(is)
{
  // This sentry addition means we never have to test objId against zero
  objMap[0] = { nullptr, nullptr };
}

IArchiveImpl::IArchiveImpl(std::istream& is) :
  is(*new InputStreamAdapter{ is }),
  pIsMem(&this->is),
  pfnDtor([](void* ptr) {
    delete (InputStreamAdapter*)ptr;
  })
{
  // This sentry addition means we never have to test objId against zero
  objMap[0] = { nullptr, nullptr };
}

IArchiveImpl::~IArchiveImpl(void) {
  if(pfnDtor)
    pfnDtor(pIsMem);
  ClearObjectTable();
}

void IArchiveImpl::ReadObject(const field_serializer& sz, void* pObj, internal::AllocationBase* pOwner) {
  Process(
    deserialization_task(
      &sz,
      0,
      pObj
    )
  );

  // If objects exist that require transference, then we have an error
  if (!pOwner) {
    if (ClearObjectTable())
      throw std::runtime_error(
        "Attempted to perform an allocator-free deserialization on a stream whose types are not completely responsible for their own cleanup"
      );
  }
  else {
    Transfer(*pOwner);
  }
}

void* IArchiveImpl::ReadObjectReference(const create_delete& cd,  const leap::field_serializer &sz) {
    // We expect to find an ID in the input stream
  uint32_t objId;
  ReadByteArray(&objId, sizeof(objId));

  // Now we just perform a lookup into our archive and store the result here
  return Lookup(cd, sz, objId);
}

IArchive::ReleasedMemory IArchiveImpl::ReadObjectReferenceResponsible(ReleasedMemory(*pfnAlloc)(), const field_serializer& sz, bool isUnique) {
  // Object ID, then directed registration:
  uint32_t objId;
  ReadByteArray(&objId, sizeof(objId));

  // Verify no unique pointer aliases:
  if (isUnique && IsReleased(objId))
    throw std::runtime_error("Attempted to map the same object into two distinct unique pointers");

  return Release(pfnAlloc, sz, objId);
}

void* IArchiveImpl::Lookup(const create_delete& cd, const field_serializer& serializer, uint32_t objId) {
  auto q = objMap.find(objId);
  if (q != objMap.end())
    return q->second.pObject;

  // Not yet initialized, allocate and queue up
  auto& entry = objMap[objId];
  entry.pObject = cd.pfnAlloc();
  entry.pfnFree = cd.pfnFree;
  work.push(deserialization_task(&serializer, objId, entry.pObject));
  return entry.pObject;
}

void IArchiveImpl::ReadDescriptor(const descriptor& descriptor, void* pObj, uint64_t ncb) {
  uint64_t countLimit = Count() + ncb;
  for (const auto& field_descriptor : descriptor.field_descriptors)
    field_descriptor.serializer.deserialize(
      *this,
      static_cast<char*>(pObj)+field_descriptor.offset,
      0
    );

  if (!ncb)
    // Impossible for there to be more fields, we don't have a sizer
    return;

  // Identified fields, read them in
  while (Count() < countLimit) {
    // Ident/type field first
    uint64_t ident = ReadInteger(sizeof(uint64_t));
    uint64_t ncbChild;
    switch ((Protobuf::serial_type)(ident & 7)) {
    case Protobuf::serial_type::string:
      ncbChild = ReadInteger(sizeof(uint64_t));
      break;
    case Protobuf::serial_type::b64:
      ncbChild = 8;
      break;
    case Protobuf::serial_type::b32:
      ncbChild = 4;
      break;
    case Protobuf::serial_type::varint:
      ncbChild = 0;
      break;
    default:
      throw std::runtime_error("Unexpected type field encountered");
    }

    // See if we can find the descriptor for this field:
    auto q = descriptor.identified_descriptors.find(ident >> 3);
    if (q == descriptor.identified_descriptors.end())
      // Unrecognized field, need to skip
      if (static_cast<Protobuf::serial_type>(ident & 7) == Protobuf::serial_type::varint)
        // Just read a varint in that we discard right away
        ReadInteger(sizeof(uint64_t));
      else
        // Skip the requisite number of bytes
        Skip(static_cast<size_t>(ncbChild));
    else
      // Hand off to child class
      q->second.serializer.deserialize(
        *this,
        static_cast<char*>(pObj)+q->second.offset,
        static_cast<size_t>(ncbChild)
      );
  }

  if (Count() > countLimit) {
    std::ostringstream os;
    os << "Deserialization error, read " << (Count() - countLimit + ncb) << " bytes and expected to read " << ncb << " bytes";
    throw std::runtime_error(os.str());
  }
}

void IArchiveImpl::ReadArray(IArrayAppender&& ary) {
  // Read the number of entries first:
  uint32_t nEntries;
  ReadByteArray(&nEntries, sizeof(nEntries));
  uint32_t n = nEntries & 0x7FFFFFFF;
  ary.reserve(n);

  if(nEntries & 0x80000000) {
    // Counted-size fields
    for (size_t i = n; i--;)
      ary.serializer.deserialize(
        *this,
        ary.allocate(),
        ReadInteger(8)
      );
  }
  else {
    // Fixed-size fields, just read everything in
    for (size_t i = n; i--;)
      ary.serializer.deserialize(*this, ary.allocate(), 0);
  }
}

void IArchiveImpl::ReadString(std::function<void*(uint64_t)> getBufferFn, uint8_t charSize, uint64_t ncb) {
  // Read the number of entries first:
  uint32_t nEntries;
  ReadByteArray(&nEntries, sizeof(nEntries));

  auto* pBuf = getBufferFn(nEntries);

  ReadByteArray(pBuf, nEntries * charSize);
}

void IArchiveImpl::ReadDictionary(IDictionaryInserter&& dictionary)
{
  // Read the number of entries first:
  uint32_t nEntries;
  ReadByteArray(&nEntries, sizeof(nEntries));

  // Now read in all values:
  while (nEntries--) {
    dictionary.key_serializer.deserialize(*this, dictionary.key(), 0);
    void* value = dictionary.insert();
    dictionary.value_serializer.deserialize(*this, value, 0);
  }
}

IArchive::ReleasedMemory IArchiveImpl::Release(ReleasedMemory(*pfnAlloc)(), const field_serializer& serializer, uint32_t objId) {
  auto q = objMap.find(objId);
  if (q != objMap.end()) {
    // Object already allocated, we just need to remove control back to ourselves
    q->second.pfnFree = nullptr;
    return {q->second.pObject, q->second.pContext};
  }

  // Not yet initialized, allocate and queue up
  auto& entry = objMap[objId];
  IArchive::ReleasedMemory retVal = pfnAlloc();
  entry.pObject = retVal.pObject;
  entry.pContext = retVal.pContext;
  entry.pfnFree = nullptr;
  work.push(deserialization_task(&serializer, objId, entry.pObject));
  return retVal;
}

bool IArchiveImpl::IsReleased(uint32_t objId) {
  auto q = objMap.find(objId);
  return q != objMap.end() && !q->second.pfnFree;
}

void IArchiveImpl::ReadByteArray(void* pBuf, uint64_t ncb) {
  std::streamsize nRead = is.Read(pBuf, ncb);
  if(nRead != ncb)
    throw std::runtime_error("End of file reached prematurely");
  m_count += ncb;
}

void IArchiveImpl::Skip(uint64_t ncb) {
  is.Skip(ncb);
}

void IArchiveImpl::Transfer(internal::AllocationBase& alloc) {
  for (auto& cur : objMap)
    if (cur.second.pfnFree)
      // Transfer cleanup responsibility to the allocator
      alloc.garbageList.push_back(
        std::make_pair(
          cur.second.pObject,
          cur.second.pfnFree
        )
      );

  objMap.clear();
}

bool IArchiveImpl::ReadBool() {
  bool b;
  ReadByteArray(&b, 1);
  return b;
}

uint64_t IArchiveImpl::ReadInteger(uint8_t) {
  size_t ncb = 0;
  uint8_t buf[10];
  do ReadByteArray(&buf[ncb], 1);
  while (buf[ncb++] & 0x80);
  return leap::FromBase128(buf, ncb);
}

size_t IArchiveImpl::ClearObjectTable(void) {
  size_t n = 0;
  for (auto& cur : objMap)
    if (cur.second.pfnFree) {
      // One more entry freed
      n++;

      // Transfer cleanup responsibility to the allocator
      cur.second.pfnFree(cur.second.pObject);
    }

  objMap.clear();
  return n;
}

void IArchiveImpl::Process(const deserialization_task& task) {
  objMap[1] = {task.pObject, nullptr};
  work.push(task);

  // Continue to work as long as there is work to be done
  for(; !work.empty(); work.pop()) {
    deserialization_task& task = work.front();

    // Identifier/type comes first
    auto id_type = ReadInteger(8);

    // Then we need the size (if it's available)
    uint64_t ncb = 0;
    switch (static_cast<Protobuf::serial_type>(id_type & 7)) {
    case Protobuf::serial_type::b32:
      ncb = 4;
      break;
    case Protobuf::serial_type::b64:
      ncb = 8;
      break;
    case Protobuf::serial_type::string:
      // Size fits right here
      ncb = ReadInteger(sizeof(ncb));
      break;
    case Protobuf::serial_type::varint:
    case Protobuf::serial_type::ignored:
      break;
    }

    task.serializer->deserialize(*this, task.pObject, ncb);
  }
}
