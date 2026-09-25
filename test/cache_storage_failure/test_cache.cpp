#include <BookMetadataCache.h>
#include <Serialization.h>

#include <cassert>
#include <iostream>
#include <sstream>
template <class T>
void pod(T v) {
  const auto p = reinterpret_cast<const uint8_t*>(&v);
  disk->bytes.insert(disk->bytes.end(), p, p + sizeof(v));
}
void str(const std::string& v) {
  pod<uint32_t>(v.size());
  disk->bytes.insert(disk->bytes.end(), v.begin(), v.end());
}
void fixture() {
  disk = std::make_shared<TestDisk>();
  pod<uint8_t>(11);
  pod<uint32_t>(37);
  pod<uint16_t>(1);
  pod<uint16_t>(1);
  for (int i = 0; i < 7; ++i) str("");
  pod<uint32_t>(45);
  pod<uint32_t>(60);
  str("a.htm");
  pod<uint32_t>(123);
  pod<int16_t>(0);
  str("Title");
  str("a.htm");
  str("");
  pod<uint8_t>(0);
  pod<int16_t>(0);
}
int main() {
  // A default HAL handle has no implementation. Failed opens create a closed
  // implementation, and close/read/seek on a never-opened handle assert.
  HalFile unopened;
  assert(!unopened);
  fixture();
  {
    BookMetadataCache fresh("/fresh");
    assert(fresh.load());
    assert(fresh.getSpineEntry(0).href == "a.htm");
  }
  {
    BookMetadataCache absent("/absent");
    disk->removed = true;
    assert(!absent.load());
    assert(!absent.isLoaded());
    assert(!absent.load());
    assert(absent.getSpineEntry(0).href.empty());
    fixture();
    assert(absent.load());
    assert(absent.getSpineEntry(0).href == "a.htm");
  }
  fixture();
  BookMetadataCache cache("/book");
  assert(cache.load());
  assert(cache.getSpineEntry(0).href == "a.htm");
  assert(cache.getTocEntry(0).title == "Title");
  disk->removed = true;
  assert(cache.getSpineEntry(0).href.empty());
  assert(!cache.isLoaded());
  fixture();
  assert(cache.load());
  disk->failSeek = true;
  assert(cache.getTocEntry(0).title.empty());
  assert(!cache.isLoaded());
  // Remove the card at each read boundary, including after the length succeeds.
  for (int fail = 0; fail < 5; ++fail) {
    fixture();
    assert(cache.load());
    disk->failRead = fail;
    auto entry = cache.getSpineEntry(0);
    assert(entry.href.empty());
    assert(!cache.isLoaded());
  }
  for (int fail = 0; fail < 8; ++fail) {
    fixture();
    assert(cache.load());
    disk->failRead = fail;
    auto entry = cache.getTocEntry(0);
    assert(entry.title.empty());
    assert(!cache.isLoaded());
  }
  fixture();
  auto valid = disk->bytes;
  for (size_t size = 0; size < 60; ++size) {
    fixture();
    disk->bytes.resize(size);
    assert(!cache.load());
    assert(!cache.isLoaded());
  }
  fixture();
  disk->bytes[37] = 0;
  assert(cache.load());
  assert(cache.getSpineEntry(0).href.empty());
  assert(!cache.isLoaded());
  fixture();
  disk->bytes[45] = 0xff;
  disk->bytes[46] = 0xff;
  disk->bytes[47] = 0xff;
  disk->bytes[48] = 0xff;
  assert(!cache.load());
  for (bool buffered : {false, true}) {
    for (uint32_t len : {0u, 5u, 16385u, 0xffffffffu}) {
      disk = std::make_shared<TestDisk>();
      pod<uint32_t>(len);
      HalFile f{true};
      std::string out = "stale";
      if (buffered) {
        serialization::BufferedFileReader r(f, 8);
        assert(serialization::readString(r, out) == (len == 0));
      } else
        assert(serialization::readString(f, out) == (len == 0));
      assert(out.empty());
    }
  }
  // An oversized field must be rejected even when the file really contains it.
  disk = std::make_shared<TestDisk>();
  pod<uint32_t>(16385);
  disk->bytes.resize(16389, 'x');
  HalFile oversized{true};
  std::string limited = "old";
  assert(!serialization::readString(oversized, limited));
  assert(limited.empty());
  assert(oversized.position() == sizeof(uint32_t));
  // Partial scalar reads never leave a partially overwritten value behind.
  disk = std::make_shared<TestDisk>();
  pod<uint8_t>(0xff);
  HalFile shortPod{true};
  uint32_t value = 123;
  assert(!serialization::readPod(shortPod, value));
  assert(value == 0);
  HalFile shortBufferedPod{true};
  serialization::BufferedFileReader bufferedPod(shortBufferedPod, 8);
  value = 123;
  assert(!serialization::readPod(bufferedPod, value));
  assert(value == 0);
  std::istringstream truncated(std::string("\xff", 1));
  std::string out = "stale";
  assert(!serialization::readString(truncated, out));
  assert(out.empty());
  fixture();
  assert(cache.load());
  assert(cache.getSpineEntry(0).href == "a.htm");
  std::cout << "Cache storage failure: removal, short reads, bad seeks, corrupt lengths/LUT, and reload passed\n";
}
