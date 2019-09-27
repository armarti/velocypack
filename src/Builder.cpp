////////////////////////////////////////////////////////////////////////////////
/// @brief Library to build up VPack documents.
///
/// DISCLAIMER
///
/// Copyright 2015 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
/// @author Jan Steemann
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include <unordered_set>
#include <random>
#include <cmath>

#include "velocypack/velocypack-common.h"
#include "velocypack/Builder.h"
#include "velocypack/Dumper.h"
#include "velocypack/Iterator.h"
#include "velocypack/Sink.h"

using namespace arangodb::velocypack;

std::string Builder::toString() const {
  Options options;
  options.prettyPrint = true;

  std::string buffer;
  StringSink sink(&buffer);
  Dumper::dump(slice(), &sink, &options);
  return buffer;
}

std::string Builder::toJson() const {
  std::string buffer;
  StringSink sink(&buffer);
  Dumper::dump(slice(), &sink);
  return buffer;
}

uint8_t const* Builder::findAttrName(uint8_t const* base, uint64_t& len) {
  uint8_t const b = *base;
  if (b >= 0x40 && b <= 0xbe) {
    // short UTF-8 string
    len = b - 0x40;
    return base + 1;
  }
  if (b == 0xbf) {
    // long UTF-8 string
    len = 0;
    // read string length
    for (size_t i = 8; i >= 1; i--) {
      len = (len << 8) + base[i];
    }
    return base + 1 + 8;  // string starts here
  }

  // translate attribute name
  return findAttrName(Slice(base).makeKey().start(), len);
}

void Builder::removeLast() {
  if (_stack.empty()) {
    throw Exception(Exception::BuilderNeedOpenCompound);
  }
  ValueLength& tos = _stack.back();
  std::vector<ValueLength>& index = _index[_stack.size() - 1];
  if (index.empty()) {
    throw Exception(Exception::BuilderNeedSubvalue);
  }
  _pos = tos + index.back();
  index.pop_back();
}

Builder& Builder::closeEmptyArrayOrObject(ValueLength tos, bool isArray) {
  // empty Array or Object
  _start[tos] = (isArray ? 0x01 : 0x0a);
  VELOCYPACK_ASSERT(_pos == tos + 9);
  _pos -= 8;  // no bytelength and number subvalues needed
  _stack.pop_back();
  // Intentionally leave _index[depth] intact to avoid future allocs!
  return *this;
}

bool Builder::closeCompactArrayOrObject(ValueLength tos, bool isArray,
                                        std::vector<ValueLength> const& index) {
  // use compact notation
  ValueLength nLen =
      getVariableValueLength(static_cast<ValueLength>(index.size()));
  VELOCYPACK_ASSERT(nLen > 0);
  ValueLength byteSize = _pos - (tos + 8) + nLen;
  VELOCYPACK_ASSERT(byteSize > 0);
  ValueLength bLen = getVariableValueLength(byteSize);
  byteSize += bLen;
  if (getVariableValueLength(byteSize) != bLen) {
    byteSize += 1;
    bLen += 1;
  }

  if (bLen < 9) {
    // can only use compact notation if total byte length is at most 8 bytes
    // long
    _start[tos] = (isArray ? 0x13 : 0x14);
    ValueLength targetPos = 1 + bLen;

    if (_pos > (tos + 9)) {
      ValueLength len = _pos - (tos + 9);
      memmove(_start + tos + targetPos, _start + tos + 9, checkOverflow(len));
    }

    // store byte length
    VELOCYPACK_ASSERT(byteSize > 0);
    storeVariableValueLength<false>(_start + tos + 1, byteSize);

    // need additional memory for storing the number of values
    if (nLen > 8 - bLen) {
      reserveSpace(nLen);
    }
    storeVariableValueLength<true>(_start + tos + byteSize - 1,
                                   static_cast<ValueLength>(index.size()));

    _pos -= 8;
    _pos += nLen + bLen;

    _stack.pop_back();
    return true;
  }
  return false;
}

Builder& Builder::closeArray(ValueLength tos, std::vector<ValueLength>& index) {
  // fix head byte in case a compact Array was originally requested:
  _start[tos] = 0x06;

  bool needIndexTable = true;
  bool needNrSubs = true;
  if (index.size() == 1) {
    needIndexTable = false;
    needNrSubs = false;
  } else if ((_pos - tos) - index[0] == index.size() * (index[1] - index[0])) {
    // In this case it could be that all entries have the same length
    // and we do not need an offset table at all:
    bool noTable = true;
    ValueLength const subLen = index[1] - index[0];
    if ((_pos - tos) - index[index.size() - 1] != subLen) {
      noTable = false;
    } else {
      for (size_t i = 1; i < index.size() - 1; i++) {
        if (index[i + 1] - index[i] != subLen) {
          noTable = false;
          break;
        }
      }
    }
    if (noTable) {
      needIndexTable = false;
      needNrSubs = false;
    }
  }

  // First determine byte length and its format:
  unsigned int offsetSize;
  // can be 1, 2, 4 or 8 for the byte width of the offsets,
  // the byte length and the number of subvalues:
  if (_pos - tos + (needIndexTable ? index.size() : 0) - (needNrSubs ? 6 : 7) <=
      0xff) {
    // We have so far used _pos - tos bytes, including the reserved 8
    // bytes for byte length and number of subvalues. In the 1-byte number
    // case we would win back 6 bytes but would need one byte per subvalue
    // for the index table
    offsetSize = 1;
  } else if (_pos - tos + (needIndexTable ? 2 * index.size() : 0) <= 0xffff) {
    offsetSize = 2;
  } else if (_pos - tos + (needIndexTable ? 4 * index.size() : 0) <=
             0xffffffffu) {
    offsetSize = 4;
  } else {
    offsetSize = 8;
  }

  // Maybe we need to move down data:
  if (offsetSize == 1) {
    ValueLength targetPos = 3;
    if (!needIndexTable) {
      targetPos = 2;
    }
    if (_pos > (tos + 9)) {
      ValueLength len = _pos - (tos + 9);
      memmove(_start + tos + targetPos, _start + tos + 9, checkOverflow(len));
    }
    ValueLength const diff = 9 - targetPos;
    _pos -= diff;
    if (needIndexTable) {
      size_t const n = index.size();
      for (size_t i = 0; i < n; i++) {
        index[i] -= diff;
      }
    }  // Note: if !needIndexTable the index array is now wrong!
  }
  // One could move down things in the offsetSize == 2 case as well,
  // since we only need 4 bytes in the beginning. However, saving these
  // 4 bytes has been sacrificed on the Altar of Performance.

  // Now build the table:
  if (needIndexTable) {
    ValueLength tableBase;
    reserveSpace(offsetSize * index.size() + (offsetSize == 8 ? 8 : 0));
    tableBase = _pos;
    _pos += offsetSize * index.size();
    for (size_t i = 0; i < index.size(); i++) {
      uint64_t x = index[i];
      for (size_t j = 0; j < offsetSize; j++) {
        _start[tableBase + offsetSize * i + j] = x & 0xff;
        x >>= 8;
      }
    }
  } else {  // no index table
    _start[tos] = 0x02;
  }
  // Finally fix the byte width in the type byte:
  if (offsetSize > 1) {
    if (offsetSize == 2) {
      _start[tos] += 1;
    } else if (offsetSize == 4) {
      _start[tos] += 2;
    } else {  // offsetSize == 8
      _start[tos] += 3;
      if (needNrSubs) {
        appendLength(index.size(), 8);
      }
    }
  }

  // Fix the byte length in the beginning:
  ValueLength x = _pos - tos;
  for (unsigned int i = 1; i <= offsetSize; i++) {
    _start[tos + i] = x & 0xff;
    x >>= 8;
  }

  if (offsetSize < 8 && needNrSubs) {
    x = index.size();
    for (unsigned int i = offsetSize + 1; i <= 2 * offsetSize; i++) {
      _start[tos + i] = x & 0xff;
      x >>= 8;
    }
  }

  // Now the array or object is complete, we pop a ValueLength
  // off the _stack:
  _stack.pop_back();
  // Intentionally leave _index[depth] intact to avoid future allocs!
  return *this;
}

Builder& Builder::close() {
  if (isClosed()) {
    throw Exception(Exception::BuilderNeedOpenCompound);
  }
  ValueLength tos = _stack.back();
  uint8_t const head = _start[tos];

  VELOCYPACK_ASSERT(head == 0x06 || head == 0x0b || head == 0x13 ||
                    head == 0x14);

  bool const isArray = (head == 0x06 || head == 0x13);
  std::vector<ValueLength>& index = _index[_stack.size() - 1];

  if (index.empty()) {
    return closeEmptyArrayOrObject(tos, isArray);
  }

  // From now on index.size() > 0
  VELOCYPACK_ASSERT(index.size() > 0);

  // check if we can use the compact Array / Object format
  if (head == 0x13 || head == 0x14 ||
      (head == 0x06 && options->buildUnindexedArrays) ||
      (head == 0x0b && (options->buildUnindexedObjects || index.size() == 1))) {
    if (closeCompactArrayOrObject(tos, isArray, index)) {
      return *this;
    }
    // This might fall through, if closeCompactArrayOrObject gave up!
  }

  if (isArray) {
    return closeArray(tos, index);
  }

  // fix head byte in case a compact Array / Object was originally requested
  _start[tos] = 0x0b;

  // Now create the hash table to see how long the object will actually be:
  std::vector<ValueLength> ht;
  uint8_t seed;
  ValueLength nrSlots = computeCuckooHash(ht, seed);

  // First determine byte length and its format:
  unsigned int offsetSize;
  // can be 1, 2, 4 or 8 for the byte width of the offsets,
  // the byte length and the number of subvalues:
  if (_pos - tos + nrSlots - 4 <= 0xff) {
    // We have so far used _pos - tos bytes, including the reserved 8
    // bytes for byte length and number of subvalues. In the 1-byte number
    // case we would win back 4 bytes but would need one byte per subvalue
    // for the index table
    offsetSize = 1;
  } else if (_pos - tos + 2 * nrSlots <= 0xffff) {
    offsetSize = 2;
  } else if (_pos - tos + 4 * nrSlots <= 0xffffffffu) {
    offsetSize = 4;
  } else {
    offsetSize = 8;
  }

  // Maybe we need to move down data:
  if (offsetSize == 1) {
    if (_pos > (tos + 9)) {
      ValueLength len = _pos - (tos + 9);
      memmove(_start + tos + 5, _start + tos + 9, checkOverflow(len));
    }
    ValueLength const diff = 4;
    _pos -= diff;
    size_t const n = index.size();
    for (size_t i = 0; i < n; i++) {
      index[i] -= diff;
    }
    for (size_t i = 0; i < nrSlots; ++i) {
      if (ht[i] != 0) {
        ht[i] -= diff;
      }
    }
  }
  // One could move down things in the offsetSize == 2 case as well,
  // since we only need 7 bytes in the beginning. However, saving these
  // 1 byte has been sacrificed on the Altar of Performance.

  // Now build the table:
  ValueLength tableBase;
  reserveSpace(offsetSize * nrSlots + (offsetSize == 8 ? 17 : 0)
                                    + (offsetSize == 4 ? 5  : 0));
  tableBase = _pos;
  _pos += offsetSize * nrSlots;
  // Object
  for (size_t i = 0; i < nrSlots; i++) {
    uint64_t x = ht[i];
    for (size_t j = 0; j < offsetSize; j++) {
      _start[tableBase + offsetSize * i + j] = x & 0xff;
      x >>= 8;
    }
  }
  // Finally fix the byte width in the type byte:
  if (offsetSize > 1) {
    if (offsetSize == 2) {
      _start[tos] = 0x0c;
    } else if (offsetSize == 4) {
      _start[tos] = 0x0d;
      appendLength(nrSlots, 4);
      appendLength(seed, 1);
    } else {  // offsetSize == 8
      _start[tos] = 0x0e;
      appendLength(index.size(), 8);
      appendLength(nrSlots, 8);
      appendLength(seed, 1);
    }
  }

  // Fix the byte length in the beginning:
  ValueLength x = _pos - tos;
  for (unsigned int i = 1; i <= offsetSize; i++) {
    _start[tos + i] = x & 0xff;
    x >>= 8;
  }

  // Add number of entries, nrSlots and seed, if they are in the front:
  if (offsetSize < 8) {
    x = index.size();
    for (unsigned int i = offsetSize + 1; i <= 2 * offsetSize; i++) {
      _start[tos + i] = x & 0xff;
      x >>= 8;
    }
    if (offsetSize < 4) {
      x = nrSlots;
      unsigned int base = (offsetSize == 1) ? 3 : 5;
      for (unsigned int i = base; i < base + offsetSize; i++) {
        _start[tos + i ] = x & 0xff;
        x >>= 8;
      }
      _start[base + offsetSize] = seed;
    }
  }

  // Now the array or object is complete, we pop a ValueLength
  // off the _stack:
  _stack.pop_back();
  // Intentionally leave _index[depth] intact to avoid future allocs!
  return *this;
}

// checks whether an Object value has a specific key attribute
bool Builder::hasKey(std::string const& key) const {
  if (_stack.empty()) {
    throw Exception(Exception::BuilderNeedOpenObject);
  }
  ValueLength const& tos = _stack.back();
  if (_start[tos] != 0x0b && _start[tos] != 0x14) {
    throw Exception(Exception::BuilderNeedOpenObject);
  }
  std::vector<ValueLength> const& index = _index[_stack.size() - 1];
  if (index.empty()) {
    return false;
  }
  for (size_t i = 0; i < index.size(); ++i) {
    Slice s(_start + tos + index[i]);
    if (s.makeKey().isEqualString(key)) {
      return true;
    }
  }
  return false;
}

// return the value for a specific key of an Object value
Slice Builder::getKey(std::string const& key) const {
  if (_stack.empty()) {
    throw Exception(Exception::BuilderNeedOpenObject);
  }
  ValueLength const tos = _stack.back();
  if (_start[tos] != 0x0b && _start[tos] != 0x14) {
    throw Exception(Exception::BuilderNeedOpenObject);
  }
  std::vector<ValueLength> const& index = _index[_stack.size() - 1];
  if (index.empty()) {
    return Slice();
  }
  for (size_t i = 0; i < index.size(); ++i) {
    Slice s(_start + tos + index[i]);
    if (s.makeKey().isEqualString(key)) {
      return Slice(s.start() + s.byteSize());
    }
  }
  return Slice();
}

uint8_t* Builder::set(Value const& item) {
  auto const oldPos = _pos;
  auto ctype = item.cType();

  checkKeyIsString(item.valueType() == ValueType::String);

  // This method builds a single further VPack item at the current
  // append position. If this is an array or object, then an index
  // table is created and a new ValueLength is pushed onto the stack.
  switch (item.valueType()) {
    case ValueType::None: {
      throw Exception(Exception::BuilderUnexpectedType,
                      "Cannot set a ValueType::None");
    }
    case ValueType::Null: {
      reserveSpace(1);
      _start[_pos++] = 0x18;
      break;
    }
    case ValueType::Bool: {
      if (ctype != Value::CType::Bool) {
        throw Exception(Exception::BuilderUnexpectedValue,
                        "Must give bool for ValueType::Bool");
      }
      reserveSpace(1);
      if (item.getBool()) {
        _start[_pos++] = 0x1a;
      } else {
        _start[_pos++] = 0x19;
      }
      break;
    }
    case ValueType::Double: {
      static_assert(sizeof(double) == sizeof(uint64_t),
                    "size of double is not 8 bytes");
      double v = 0.0;
      uint64_t x;
      switch (ctype) {
        case Value::CType::Double:
          v = item.getDouble();
          break;
        case Value::CType::Int64:
          v = static_cast<double>(item.getInt64());
          break;
        case Value::CType::UInt64:
          v = static_cast<double>(item.getUInt64());
          break;
        default:
          throw Exception(Exception::BuilderUnexpectedValue,
                          "Must give number for ValueType::Double");
      }
      reserveSpace(1 + sizeof(double));
      _start[_pos++] = 0x1b;
      memcpy(&x, &v, sizeof(double));
      appendLength(x, 8);
      break;
    }
    case ValueType::External: {
      if (options->disallowExternals) {
        // External values explicitly disallowed as a security
        // precaution
        throw Exception(Exception::BuilderExternalsDisallowed);
      }
      if (ctype != Value::CType::VoidPtr) {
        throw Exception(Exception::BuilderUnexpectedValue,
                        "Must give void pointer for ValueType::External");
      }
      reserveSpace(1 + sizeof(void*));
      // store pointer. this doesn't need to be portable
      _start[_pos++] = 0x1d;
      void const* value = item.getExternal();
      memcpy(_start + _pos, &value, sizeof(void*));
      _pos += sizeof(void*);
      break;
    }
    case ValueType::SmallInt: {
      int64_t vv = 0;
      switch (ctype) {
        case Value::CType::Double:
          vv = static_cast<int64_t>(item.getDouble());
          break;
        case Value::CType::Int64:
          vv = item.getInt64();
          break;
        case Value::CType::UInt64:
          vv = static_cast<int64_t>(item.getUInt64());
          break;
        default:
          throw Exception(Exception::BuilderUnexpectedValue,
                          "Must give number for ValueType::SmallInt");
      }
      if (vv < -6 || vv > 9) {
        throw Exception(Exception::NumberOutOfRange,
                        "Number out of range of ValueType::SmallInt");
      }
      reserveSpace(1);
      if (vv >= 0) {
        _start[_pos++] = static_cast<uint8_t>(vv + 0x30);
      } else {
        _start[_pos++] = static_cast<uint8_t>(vv + 0x40);
      }
      break;
    }
    case ValueType::Int: {
      int64_t v;
      switch (ctype) {
        case Value::CType::Double:
          v = static_cast<int64_t>(item.getDouble());
          break;
        case Value::CType::Int64:
          v = item.getInt64();
          break;
        case Value::CType::UInt64:
          v = toInt64(item.getUInt64());
          break;
        default:
          throw Exception(Exception::BuilderUnexpectedValue,
                          "Must give number for ValueType::Int");
      }
      addInt(v);
      break;
    }
    case ValueType::UInt: {
      uint64_t v = 0;
      switch (ctype) {
        case Value::CType::Double:
          if (item.getDouble() < 0.0) {
            throw Exception(
                Exception::BuilderUnexpectedValue,
                "Must give non-negative number for ValueType::UInt");
          }
          v = static_cast<uint64_t>(item.getDouble());
          break;
        case Value::CType::Int64:
          if (item.getInt64() < 0) {
            throw Exception(
                Exception::BuilderUnexpectedValue,
                "Must give non-negative number for ValueType::UInt");
          }
          v = static_cast<uint64_t>(item.getInt64());
          break;
        case Value::CType::UInt64:
          v = item.getUInt64();
          break;
        default:
          throw Exception(Exception::BuilderUnexpectedValue,
                          "Must give number for ValueType::UInt");
      }
      addUInt(v);
      break;
    }
    case ValueType::UTCDate: {
      int64_t v;
      switch (ctype) {
        case Value::CType::Double:
          v = static_cast<int64_t>(item.getDouble());
          break;
        case Value::CType::Int64:
          v = item.getInt64();
          break;
        case Value::CType::UInt64:
          v = toInt64(item.getUInt64());
          break;
        default:
          throw Exception(Exception::BuilderUnexpectedValue,
                          "Must give number for ValueType::UTCDate");
      }
      addUTCDate(v);
      break;
    }
    case ValueType::String: {
      std::string const* s;
      std::string value;
      if (ctype == Value::CType::String) {
        s = item.getString();
      } else if (ctype == Value::CType::CharPtr) {
        value = item.getCharPtr();
        s = &value;
      } else {
        throw Exception(
            Exception::BuilderUnexpectedValue,
            "Must give a string or char const* for ValueType::String");
      }
      size_t const size = s->size();
      if (size <= 126) {
        // short string
        reserveSpace(1 + size);
        _start[_pos++] = static_cast<uint8_t>(0x40 + size);
        memcpy(_start + _pos, s->c_str(), size);
      } else {
        // long string
        reserveSpace(1 + 8 + size);
        _start[_pos++] = 0xbf;
        appendLength(size, 8);
        memcpy(_start + _pos, s->c_str(), size);
      }
      _pos += size;
      break;
    }
    case ValueType::Array: {
      addArray(item._unindexed);
      break;
    }
    case ValueType::Object: {
      addObject(item._unindexed);
      break;
    }
    case ValueType::Binary: {
      if (ctype != Value::CType::String && ctype != Value::CType::CharPtr) {
        throw Exception(
            Exception::BuilderUnexpectedValue,
            "Must provide std::string or char const* for ValueType::Binary");
      }
      std::string const* s;
      std::string value;
      if (ctype == Value::CType::String) {
        s = item.getString();
      } else {
        value = item.getCharPtr();
        s = &value;
      }
      ValueLength v = s->size();
      reserveSpace(9 + v);
      appendUInt(v, 0xbf);
      memcpy(_start + _pos, s->c_str(), checkOverflow(v));
      _pos += v;
      break;
    }
    case ValueType::Illegal: {
      reserveSpace(1);
      _start[_pos++] = 0x17;
      break;
    }
    case ValueType::MinKey: {
      reserveSpace(1);
      _start[_pos++] = 0x1e;
      break;
    }
    case ValueType::MaxKey: {
      reserveSpace(1);
      _start[_pos++] = 0x1f;
      break;
    }
    case ValueType::BCD: {
      throw Exception(Exception::NotImplemented);
    }
    case ValueType::Custom: {
      throw Exception(Exception::BuilderUnexpectedType,
                      "Cannot set a ValueType::Custom with this method");
    }
  }
  return _start + oldPos;
}

uint8_t* Builder::set(Slice const& item) {
  checkKeyIsString(item.isString());

  ValueLength const l = item.byteSize();
  reserveSpace(l);
  memcpy(_start + _pos, item.start(), checkOverflow(l));
  _pos += l;
  return _start + _pos - l;
}

uint8_t* Builder::set(ValuePair const& pair) {
  // This method builds a single further VPack item at the current
  // append position. This is the case for ValueType::String,
  // ValueType::Binary, or ValueType::Custom, which can be built
  // with two pieces of information

  auto const oldPos = _pos;

  checkKeyIsString(pair.valueType() == ValueType::String);

  if (pair.valueType() == ValueType::Binary) {
    uint64_t v = pair.getSize();
    appendUInt(v, 0xbf);
    memcpy(_start + _pos, pair.getStart(), checkOverflow(v));
    _pos += v;
    return _start + oldPos;
  } else if (pair.valueType() == ValueType::String) {
    uint64_t size = pair.getSize();
    if (size > 126) {
      // long string
      reserveSpace(1 + 8 + size);
      _start[_pos++] = 0xbf;
      appendLength(size, 8);
      memcpy(_start + _pos, pair.getStart(), checkOverflow(size));
      _pos += size;
    } else {
      // short string
      reserveSpace(1 + size);
      _start[_pos++] = static_cast<uint8_t>(0x40 + size);
      memcpy(_start + _pos, pair.getStart(), checkOverflow(size));
      _pos += size;
    }
    return _start + oldPos;
  } else if (pair.valueType() == ValueType::Custom) {
    // We only reserve space here, the caller has to fill in the custom type
    uint64_t size = pair.getSize();
    reserveSpace(size);
    uint8_t const* p = pair.getStart();
    if (p != nullptr) {
      memcpy(_start + _pos, p, checkOverflow(size));
    }
    _pos += size;
    return _start + _pos - size;
  }
  throw Exception(Exception::BuilderUnexpectedType,
                  "Only ValueType::Binary, ValueType::String and "
                  "ValueType::Custom are valid for ValuePair argument");
}

uint8_t* Builder::add(std::string const& attrName, Value const& sub) {
  return addInternal<Value>(attrName, sub);
}

uint8_t* Builder::add(std::string const& attrName, ValuePair const& sub) {
  return addInternal<ValuePair>(attrName, sub);
}

uint8_t* Builder::add(std::string const& attrName, Slice const& sub) {
  return addInternal<Slice>(attrName, sub);
}
  
// Add all subkeys and subvalues into an object from an ObjectIterator
// and leaves open the object intentionally
uint8_t* Builder::add(ObjectIterator& sub) {
  return add(std::move(sub));
}

uint8_t* Builder::add(ObjectIterator&& sub) {
  if (_stack.empty()) {
    throw Exception(Exception::BuilderNeedOpenObject);
  }
  ValueLength& tos = _stack.back();
  if (_start[tos] != 0x0b && _start[tos] != 0x14) {
    throw Exception(Exception::BuilderNeedOpenObject);
  }
  if (_keyWritten) {
    throw Exception(Exception::BuilderKeyAlreadyWritten);
  }
  auto const oldPos = _pos;
  while (sub.valid()) {
    add(sub.key());
    add(sub.value());
    sub.next();
  }
  return _start + oldPos;
}

uint8_t* Builder::add(Value const& sub) { return addInternal<Value>(sub); }

uint8_t* Builder::add(ValuePair const& sub) {
  return addInternal<ValuePair>(sub);
}

uint8_t* Builder::add(Slice const& sub) { return addInternal<Slice>(sub); }

// Add all subkeys and subvalues into an object from an ArrayIterator
// and leaves open the array intentionally
uint8_t* Builder::add(ArrayIterator& sub) {
  return add(std::move(sub));
}

uint8_t* Builder::add(ArrayIterator&& sub) {
  if (_stack.empty()) {
    throw Exception(Exception::BuilderNeedOpenArray);
  }
  ValueLength& tos = _stack.back();
  if (_start[tos] != 0x06 && _start[tos] != 0x13) {
    throw Exception(Exception::BuilderNeedOpenArray);
  }
  auto const oldPos = _pos;
  while (sub.valid()) {
    add(sub.value());
    sub.next();
  }
  return _start + oldPos;
}

ValueLength Builder::computeCuckooHash(std::vector<ValueLength>& ht,
                                       uint8_t& seed) {
  std::mt19937_64 e(123456789);
  std::uniform_int_distribution<uint8_t> d(0,2);

  ValueLength tos = _stack.back();
  std::vector<ValueLength> const& index = _index[_stack.size() - 1];
  // The following is heuristics: We add one slot for sizes 2 to 6,
  // then 2 for sizes 7 to 13 and so on:
  ValueLength nrSlots = index.size() + (index.size() * 3) / 20 + 1;
  bool small = nrSlots <= 0x1000000;

  ValueLength searchLimit = nrSlots < 400 ? nrSlots * 3
      : 1200 + static_cast<ValueLength>(sqrt(nrSlots));

  auto insert = [&](uint8_t* objStart, ValueLength offset) {


    bool checkUniqueness = options->checkAttributeUniqueness;

    ValueLength count = 0;
    do {
      // Compute all three hash values:
      ValueLength pos[3];
      ValueLength attrLen;
      uint8_t const* attrName = findAttrName(objStart + offset, attrLen);

      auto doTheCheck = [&] (ValueLength otherOffset) -> void {
        ValueLength otherLen;
        uint8_t const* otherName = findAttrName(objStart + otherOffset,
                                                otherLen);
        if (attrLen == otherLen && memcmp(attrName, otherName, otherLen) == 0) {
          throw Exception(Exception::DuplicateAttributeName);
        }
      };

      //fasthash64x3(attrName, attrLen, Slice::seedTable + 3 * seed, pos);
      pos[0] = XXH64(attrName, attrLen, Slice::seedTable[3 * seed]);
      pos[1] = XXH64(attrName, attrLen, Slice::seedTable[3 * seed + 1]);
      pos[2] = XXH64(attrName, attrLen, Slice::seedTable[3 * seed + 2]);

      // On the topic of uniqueness: This program never deletes entries
      // from the hash table (except throwing the table away completely).
      // Furthermore, it puts a new entry in the first free of its three
      // possible positions. It might be moved later, but only because
      // something else is put in its place. Therefore, should an 
      // attribute name occur more than once, the second one will 
      // never see an empty slot before it sees the first one. qed.
      pos[0] = small ? fastModulo32Bit(pos[0], nrSlots) : pos[0] % nrSlots;
      if (ht[pos[0]] == 0) { ht[pos[0]] = offset; return true; } 
      else if (checkUniqueness) { doTheCheck(ht[pos[0]]); }
      pos[1] = small ? fastModulo32Bit(pos[1], nrSlots) : pos[1] % nrSlots;
      if (ht[pos[1]] == 0) { ht[pos[1]] = offset; return true; }
      else if (checkUniqueness) { doTheCheck(ht[pos[1]]); }
      pos[2] = small ? fastModulo32Bit(pos[2], nrSlots) : pos[2] % nrSlots;
      if (ht[pos[2]] == 0) { ht[pos[2]] = offset; return true; }
      else if (checkUniqueness) { doTheCheck(ht[pos[2]]); }

      // Play cuckoo:
      uint8_t i = d(e);
      ValueLength tmp = ht[pos[i]];
      ht[pos[i]] = offset;
      offset = tmp;
      checkUniqueness = false;
    } while (++count <= searchLimit);
    return false;
  };

  while (true) {   // outer loop to try table sizes
    seed = 0;
    do {
      // Will be left by return as soon as successful

      // Initialize empty hash table of given size:
      ht.clear();
      ht.reserve(nrSlots);
      ht.insert(ht.begin(), nrSlots, 0);
      bool error = false;
      for (size_t i = 0; i < index.size(); i++) {
        if (!insert(_start + tos, index[i])) {
          error = true;
          break;
        }
      }
      if (!error) {
        return nrSlots;
      }
      seed++;
    } while (seed != 0);
    nrSlots = nrSlots * 110 / 100;
    small = nrSlots <= 0x1000000;
  }
  // never reached
}

static_assert(sizeof(double) == 8, "double is not 8 bytes");
