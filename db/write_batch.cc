// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring varstring         |
//    kTypeDeletion varstring
// varstring :=
//    len: varint32
//    data: uint8[len]

#include <stdint.h>

#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/expiry.h"
#include "leveldb/write_batch.h"
#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"
#include "util/throttle.h"

namespace leveldb {

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
static const size_t kHeader = 12;

WriteBatch::WriteBatch() {
  Clear();
}

WriteBatch::~WriteBatch() { }

WriteBatch::Handler::~Handler() { }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }

  input.remove_prefix(kHeader);
  Slice key, value;
  ExpiryTime expiry;
  int found = 0;
  while (!input.empty()) {
    found++;
    ValueType tag = (ValueType)input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
            handler->Put(key, value, kTypeValue, 0);
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->Delete(key);
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      case kTypeValueWriteTime:
      case kTypeValueExplicitExpiry:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetVarint64(&input, &expiry) &&
            GetLengthPrefixedSlice(&input, &value)) {
            handler->Put(key, value, tag, expiry);
        } else {
          return Status::Corruption("bad WriteBatch Expiry");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else {
    return Status::OK();
  }
}

int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

void WriteBatch::Put(const Slice& key, const Slice& value, const KeyMetaData * meta) {
  KeyMetaData local_meta;
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (NULL!=meta)
      local_meta=*meta;
  rep_.push_back(static_cast<char>(local_meta.m_Type));
  PutLengthPrefixedSlice(&rep_, key);
  if (kTypeValueExplicitExpiry==local_meta.m_Type
      || kTypeValueWriteTime==local_meta.m_Type)
  {
      if (kTypeValueWriteTime==local_meta.m_Type && 0==local_meta.m_Expiry)
          local_meta.m_Expiry=GetTimeMinutes();
      PutVarint64(&rep_, local_meta.m_Expiry);
  }   // if
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

namespace {
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  MemTable* mem_;
  const Options * options_;

  MemTableInserter() : mem_(NULL), options_(NULL) {};

  virtual void Put(const Slice& key, const Slice& value, const ValueType &type, const ExpiryTime &expiry) {
    ValueType type_use(type);
    ExpiryTime expiry_use(expiry);

    if (NULL!=options_ && NULL!=options_->expiry_module.get())
        options_->expiry_module->MemTableInserterCallback(key, value, type_use, expiry_use);
    mem_->Add(sequence_, (ValueType)type_use, key, value, expiry_use);
    sequence_++;
  }
  virtual void Delete(const Slice& key) {
    mem_->Add(sequence_, kTypeDeletion, key, Slice(), 0);
    sequence_++;
  }
};
}  // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch* b,
                                      MemTable* memtable,
                                      const Options * options) {
  MemTableInserter inserter;
  inserter.sequence_ = WriteBatchInternal::Sequence(b);
  inserter.mem_ = memtable;
  inserter.options_ = options;
  return b->Iterate(&inserter);
}

void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

}  // namespace leveldb
