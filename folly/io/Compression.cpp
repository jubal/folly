/*
 * Copyright 2013 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "folly/io/Compression.h"

#include <lz4.h>
#include <lz4hc.h>
#include <glog/logging.h>
#include <snappy.h>
#include <snappy-sinksource.h>
#include <zlib.h>

#include "folly/Conv.h"
#include "folly/Memory.h"
#include "folly/Portability.h"
#include "folly/ScopeGuard.h"
#include "folly/io/Cursor.h"

namespace folly { namespace io {

// Ensure consistent behavior in the nullptr case
std::unique_ptr<IOBuf> Codec::compress(const IOBuf* data) {
  return !data->empty() ? doCompress(data) : IOBuf::create(0);
}

std::unique_ptr<IOBuf> Codec::uncompress(const IOBuf* data,
                                         uint64_t uncompressedLength) {
  if (uncompressedLength == UNKNOWN_UNCOMPRESSED_LENGTH) {
    if (needsUncompressedLength()) {
      throw std::invalid_argument("Codec: uncompressed length required");
    }
  } else if (uncompressedLength > maxUncompressedLength()) {
    throw std::runtime_error("Codec: uncompressed length too large");
  }

  if (data->empty()) {
    if (uncompressedLength != UNKNOWN_UNCOMPRESSED_LENGTH &&
        uncompressedLength != 0) {
      throw std::runtime_error("Codec: invalid uncompressed length");
    }
    return IOBuf::create(0);
  }

  return doUncompress(data, uncompressedLength);
}

bool Codec::needsUncompressedLength() const {
  return doNeedsUncompressedLength();
}

uint64_t Codec::maxUncompressedLength() const {
  return doMaxUncompressedLength();
}

CodecType Codec::type() const {
  return doType();
}

bool Codec::doNeedsUncompressedLength() const {
  return false;
}

uint64_t Codec::doMaxUncompressedLength() const {
  return std::numeric_limits<uint64_t>::max() - 1;
}

namespace {

/**
 * No compression
 */
class NoCompressionCodec FOLLY_FINAL : public Codec {
 public:
  static std::unique_ptr<Codec> create(int level);
  explicit NoCompressionCodec(int level);

 private:
  CodecType doType() const FOLLY_OVERRIDE;
  std::unique_ptr<IOBuf> doCompress(const IOBuf* data) FOLLY_OVERRIDE;
  std::unique_ptr<IOBuf> doUncompress(
      const IOBuf* data,
      uint64_t uncompressedLength) FOLLY_OVERRIDE;
};

std::unique_ptr<Codec> NoCompressionCodec::create(int level) {
  return make_unique<NoCompressionCodec>(level);
}

NoCompressionCodec::NoCompressionCodec(int level) {
  switch (level) {
  case COMPRESSION_LEVEL_DEFAULT:
  case COMPRESSION_LEVEL_FASTEST:
  case COMPRESSION_LEVEL_BEST:
    level = 0;
  }
  if (level != 0) {
    throw std::invalid_argument(to<std::string>(
        "NoCompressionCodec: invalid level ", level));
  }
}

CodecType NoCompressionCodec::doType() const {
  return CodecType::NO_COMPRESSION;
}

std::unique_ptr<IOBuf> NoCompressionCodec::doCompress(
    const IOBuf* data) {
  return data->clone();
}

std::unique_ptr<IOBuf> NoCompressionCodec::doUncompress(
    const IOBuf* data,
    uint64_t uncompressedLength) {
  if (uncompressedLength != UNKNOWN_UNCOMPRESSED_LENGTH &&
      data->computeChainDataLength() != uncompressedLength) {
    throw std::runtime_error(to<std::string>(
        "NoCompressionCodec: invalid uncompressed length"));
  }
  return data->clone();
}

/**
 * LZ4 compression
 */
class LZ4Codec FOLLY_FINAL : public Codec {
 public:
  static std::unique_ptr<Codec> create(int level);
  explicit LZ4Codec(int level);

 private:
  bool doNeedsUncompressedLength() const FOLLY_OVERRIDE;
  uint64_t doMaxUncompressedLength() const FOLLY_OVERRIDE;
  CodecType doType() const FOLLY_OVERRIDE;
  std::unique_ptr<IOBuf> doCompress(const IOBuf* data) FOLLY_OVERRIDE;
  std::unique_ptr<IOBuf> doUncompress(
      const IOBuf* data,
      uint64_t uncompressedLength) FOLLY_OVERRIDE;

  bool highCompression_;
};

std::unique_ptr<Codec> LZ4Codec::create(int level) {
  return make_unique<LZ4Codec>(level);
}

LZ4Codec::LZ4Codec(int level) {
  switch (level) {
  case COMPRESSION_LEVEL_FASTEST:
  case COMPRESSION_LEVEL_DEFAULT:
    level = 1;
    break;
  case COMPRESSION_LEVEL_BEST:
    level = 2;
    break;
  }
  if (level < 1 || level > 2) {
    throw std::invalid_argument(to<std::string>(
        "LZ4Codec: invalid level: ", level));
  }
  highCompression_ = (level > 1);
}

bool LZ4Codec::doNeedsUncompressedLength() const {
  return true;
}

uint64_t LZ4Codec::doMaxUncompressedLength() const {
  // From lz4.h: "Max supported value is ~1.9GB"; I wish we had something
  // more accurate.
  return 1.8 * (uint64_t(1) << 30);
}

CodecType LZ4Codec::doType() const {
  return CodecType::LZ4;
}

std::unique_ptr<IOBuf> LZ4Codec::doCompress(const IOBuf* data) {
  std::unique_ptr<IOBuf> clone;
  if (data->isChained()) {
    // LZ4 doesn't support streaming, so we have to coalesce
    clone = data->clone();
    clone->coalesce();
    data = clone.get();
  }

  auto out = IOBuf::create(LZ4_compressBound(data->length()));
  int n;
  if (highCompression_) {
    n = LZ4_compress(reinterpret_cast<const char*>(data->data()),
                     reinterpret_cast<char*>(out->writableTail()),
                     data->length());
  } else {
    n = LZ4_compressHC(reinterpret_cast<const char*>(data->data()),
                       reinterpret_cast<char*>(out->writableTail()),
                       data->length());
  }

  CHECK_GE(n, 0);
  CHECK_LE(n, out->capacity());

  out->append(n);
  return out;
}

std::unique_ptr<IOBuf> LZ4Codec::doUncompress(
    const IOBuf* data,
    uint64_t uncompressedLength) {
  std::unique_ptr<IOBuf> clone;
  if (data->isChained()) {
    // LZ4 doesn't support streaming, so we have to coalesce
    clone = data->clone();
    clone->coalesce();
    data = clone.get();
  }

  auto out = IOBuf::create(uncompressedLength);
  int n = LZ4_uncompress(reinterpret_cast<const char*>(data->data()),
                         reinterpret_cast<char*>(out->writableTail()),
                         uncompressedLength);
  if (n != data->length()) {
    throw std::runtime_error(to<std::string>(
        "LZ4 decompression returned invalid value ", n));
  }
  out->append(uncompressedLength);
  return out;
}

/**
 * Snappy compression
 */

/**
 * Implementation of snappy::Source that reads from a IOBuf chain.
 */
class IOBufSnappySource FOLLY_FINAL : public snappy::Source {
 public:
  explicit IOBufSnappySource(const IOBuf* data);
  size_t Available() const FOLLY_OVERRIDE;
  const char* Peek(size_t* len) FOLLY_OVERRIDE;
  void Skip(size_t n) FOLLY_OVERRIDE;
 private:
  size_t available_;
  io::Cursor cursor_;
};

IOBufSnappySource::IOBufSnappySource(const IOBuf* data)
  : available_(data->computeChainDataLength()),
    cursor_(data) {
}

size_t IOBufSnappySource::Available() const {
  return available_;
}

const char* IOBufSnappySource::Peek(size_t* len) {
  auto p = cursor_.peek();
  *len = p.second;
  return reinterpret_cast<const char*>(p.first);
}

void IOBufSnappySource::Skip(size_t n) {
  CHECK_LE(n, available_);
  cursor_.skip(n);
  available_ -= n;
}

class SnappyCodec FOLLY_FINAL : public Codec {
 public:
  static std::unique_ptr<Codec> create(int level);
  explicit SnappyCodec(int level);

 private:
  uint64_t doMaxUncompressedLength() const FOLLY_OVERRIDE;
  CodecType doType() const FOLLY_OVERRIDE;
  std::unique_ptr<IOBuf> doCompress(const IOBuf* data) FOLLY_OVERRIDE;
  std::unique_ptr<IOBuf> doUncompress(
      const IOBuf* data,
      uint64_t uncompressedLength) FOLLY_OVERRIDE;
};

std::unique_ptr<Codec> SnappyCodec::create(int level) {
  return make_unique<SnappyCodec>(level);
}

SnappyCodec::SnappyCodec(int level) {
  switch (level) {
  case COMPRESSION_LEVEL_FASTEST:
  case COMPRESSION_LEVEL_DEFAULT:
  case COMPRESSION_LEVEL_BEST:
    level = 1;
  }
  if (level != 1) {
    throw std::invalid_argument(to<std::string>(
        "SnappyCodec: invalid level: ", level));
  }
}

uint64_t SnappyCodec::doMaxUncompressedLength() const {
  // snappy.h uses uint32_t for lengths, so there's that.
  return std::numeric_limits<uint32_t>::max();
}

CodecType SnappyCodec::doType() const {
  return CodecType::SNAPPY;
}

std::unique_ptr<IOBuf> SnappyCodec::doCompress(const IOBuf* data) {
  IOBufSnappySource source(data);
  auto out =
    IOBuf::create(snappy::MaxCompressedLength(source.Available()));

  snappy::UncheckedByteArraySink sink(reinterpret_cast<char*>(
      out->writableTail()));

  size_t n = snappy::Compress(&source, &sink);

  CHECK_LE(n, out->capacity());
  out->append(n);
  return out;
}

std::unique_ptr<IOBuf> SnappyCodec::doUncompress(const IOBuf* data,
                                                 uint64_t uncompressedLength) {
  uint32_t actualUncompressedLength = 0;

  {
    IOBufSnappySource source(data);
    if (!snappy::GetUncompressedLength(&source, &actualUncompressedLength)) {
      throw std::runtime_error("snappy::GetUncompressedLength failed");
    }
    if (uncompressedLength != UNKNOWN_UNCOMPRESSED_LENGTH &&
        uncompressedLength != actualUncompressedLength) {
      throw std::runtime_error("snappy: invalid uncompressed length");
    }
  }

  auto out = IOBuf::create(actualUncompressedLength);

  {
    IOBufSnappySource source(data);
    if (!snappy::RawUncompress(&source,
                               reinterpret_cast<char*>(out->writableTail()))) {
      throw std::runtime_error("snappy::RawUncompress failed");
    }
  }

  out->append(actualUncompressedLength);
  return out;
}

/**
 * Zlib codec
 */
class ZlibCodec FOLLY_FINAL : public Codec {
 public:
  static std::unique_ptr<Codec> create(int level);
  explicit ZlibCodec(int level);

 private:
  CodecType doType() const FOLLY_OVERRIDE;
  std::unique_ptr<IOBuf> doCompress(const IOBuf* data) FOLLY_OVERRIDE;
  std::unique_ptr<IOBuf> doUncompress(
      const IOBuf* data,
      uint64_t uncompressedLength) FOLLY_OVERRIDE;

  std::unique_ptr<IOBuf> addOutputBuffer(z_stream* stream, uint32_t length);
  bool doInflate(z_stream* stream, IOBuf* head, uint32_t bufferLength);

  int level_;
};

std::unique_ptr<Codec> ZlibCodec::create(int level) {
  return make_unique<ZlibCodec>(level);
}

ZlibCodec::ZlibCodec(int level) {
  switch (level) {
  case COMPRESSION_LEVEL_FASTEST:
    level = 1;
    break;
  case COMPRESSION_LEVEL_DEFAULT:
    level = Z_DEFAULT_COMPRESSION;
    break;
  case COMPRESSION_LEVEL_BEST:
    level = 9;
    break;
  }
  if (level != Z_DEFAULT_COMPRESSION && (level < 0 || level > 9)) {
    throw std::invalid_argument(to<std::string>(
        "ZlibCodec: invalid level: ", level));
  }
  level_ = level;
}

CodecType ZlibCodec::doType() const {
  return CodecType::ZLIB;
}

std::unique_ptr<IOBuf> ZlibCodec::addOutputBuffer(z_stream* stream,
                                                  uint32_t length) {
  CHECK_EQ(stream->avail_out, 0);

  auto buf = IOBuf::create(length);
  buf->append(length);

  stream->next_out = buf->writableData();
  stream->avail_out = buf->length();

  return buf;
}

bool ZlibCodec::doInflate(z_stream* stream,
                          IOBuf* head,
                          uint32_t bufferLength) {
  if (stream->avail_out == 0) {
    head->prependChain(addOutputBuffer(stream, bufferLength));
  }

  int rc = inflate(stream, Z_NO_FLUSH);

  switch (rc) {
  case Z_OK:
    break;
  case Z_STREAM_END:
    return true;
  case Z_BUF_ERROR:
  case Z_NEED_DICT:
  case Z_DATA_ERROR:
  case Z_MEM_ERROR:
    throw std::runtime_error(to<std::string>(
        "ZlibCodec: inflate error: ", rc, ": ", stream->msg));
  default:
    CHECK(false) << rc << ": " << stream->msg;
  }

  return false;
}


std::unique_ptr<IOBuf> ZlibCodec::doCompress(const IOBuf* data) {
  z_stream stream;
  stream.zalloc = nullptr;
  stream.zfree = nullptr;
  stream.opaque = nullptr;

  int rc = deflateInit(&stream, level_);
  if (rc != Z_OK) {
    throw std::runtime_error(to<std::string>(
        "ZlibCodec: deflateInit error: ", rc, ": ", stream.msg));
  }

  stream.next_in = stream.next_out = nullptr;
  stream.avail_in = stream.avail_out = 0;
  stream.total_in = stream.total_out = 0;

  bool success = false;

  SCOPE_EXIT {
    int rc = deflateEnd(&stream);
    // If we're here because of an exception, it's okay if some data
    // got dropped.
    CHECK(rc == Z_OK || (!success && rc == Z_DATA_ERROR))
      << rc << ": " << stream.msg;
  };

  uint64_t uncompressedLength = data->computeChainDataLength();
  uint64_t maxCompressedLength = deflateBound(&stream, uncompressedLength);

  // Max 64MiB in one go
  constexpr uint32_t maxSingleStepLength = uint32_t(64) << 20;    // 64MiB
  constexpr uint32_t defaultBufferLength = uint32_t(4) << 20;     // 4MiB

  auto out = addOutputBuffer(
      &stream,
      (maxCompressedLength <= maxSingleStepLength ?
       maxCompressedLength :
       defaultBufferLength));

  for (auto& range : *data) {
    if (range.empty()) {
      continue;
    }

    stream.next_in = const_cast<uint8_t*>(range.data());
    stream.avail_in = range.size();

    while (stream.avail_in != 0) {
      if (stream.avail_out == 0) {
        out->prependChain(addOutputBuffer(&stream, defaultBufferLength));
      }

      rc = deflate(&stream, Z_NO_FLUSH);

      CHECK_EQ(rc, Z_OK) << stream.msg;
    }
  }

  do {
    if (stream.avail_out == 0) {
      out->prependChain(addOutputBuffer(&stream, defaultBufferLength));
    }

    rc = deflate(&stream, Z_FINISH);
  } while (rc == Z_OK);

  CHECK_EQ(rc, Z_STREAM_END) << stream.msg;

  out->prev()->trimEnd(stream.avail_out);

  success = true;  // we survived

  return out;
}

std::unique_ptr<IOBuf> ZlibCodec::doUncompress(const IOBuf* data,
                                               uint64_t uncompressedLength) {
  z_stream stream;
  stream.zalloc = nullptr;
  stream.zfree = nullptr;
  stream.opaque = nullptr;

  int rc = inflateInit(&stream);
  if (rc != Z_OK) {
    throw std::runtime_error(to<std::string>(
        "ZlibCodec: inflateInit error: ", rc, ": ", stream.msg));
  }

  stream.next_in = stream.next_out = nullptr;
  stream.avail_in = stream.avail_out = 0;
  stream.total_in = stream.total_out = 0;

  bool success = false;

  SCOPE_EXIT {
    int rc = inflateEnd(&stream);
    // If we're here because of an exception, it's okay if some data
    // got dropped.
    CHECK(rc == Z_OK || (!success && rc == Z_DATA_ERROR))
      << rc << ": " << stream.msg;
  };

  // Max 64MiB in one go
  constexpr uint32_t maxSingleStepLength = uint32_t(64) << 20;    // 64MiB
  constexpr uint32_t defaultBufferLength = uint32_t(4) << 20;     // 4MiB

  auto out = addOutputBuffer(
      &stream,
      ((uncompressedLength != UNKNOWN_UNCOMPRESSED_LENGTH &&
        uncompressedLength <= maxSingleStepLength) ?
       uncompressedLength :
       defaultBufferLength));

  bool streamEnd = false;
  for (auto& range : *data) {
    if (range.empty()) {
      continue;
    }

    stream.next_in = const_cast<uint8_t*>(range.data());
    stream.avail_in = range.size();

    while (stream.avail_in != 0) {
      if (streamEnd) {
        throw std::runtime_error(to<std::string>(
            "ZlibCodec: junk after end of data"));
      }

      streamEnd = doInflate(&stream, out.get(), defaultBufferLength);
    }
  }

  while (!streamEnd) {
    streamEnd = doInflate(&stream, out.get(), defaultBufferLength);
  }

  out->prev()->trimEnd(stream.avail_out);

  if (uncompressedLength != UNKNOWN_UNCOMPRESSED_LENGTH &&
      uncompressedLength != stream.total_out) {
    throw std::runtime_error(to<std::string>(
        "ZlibCodec: invalid uncompressed length"));
  }

  success = true;  // we survived

  return out;
}

typedef std::unique_ptr<Codec> (*CodecFactory)(int);

CodecFactory gCodecFactories[
    static_cast<size_t>(CodecType::NUM_CODEC_TYPES)] = {
  NoCompressionCodec::create,
  LZ4Codec::create,
  SnappyCodec::create,
  ZlibCodec::create
};

}  // namespace

std::unique_ptr<Codec> getCodec(CodecType type, int level) {
  size_t idx = static_cast<size_t>(type);
  if (idx >= static_cast<size_t>(CodecType::NUM_CODEC_TYPES)) {
    throw std::invalid_argument(to<std::string>(
        "Compression type ", idx, " not supported"));
  }
  auto factory = gCodecFactories[idx];
  if (!factory) {
    throw std::invalid_argument(to<std::string>(
        "Compression type ", idx, " not supported"));
  }
  auto codec = (*factory)(level);
  DCHECK_EQ(static_cast<size_t>(codec->type()), idx);
  return codec;
}

}}  // namespaces

