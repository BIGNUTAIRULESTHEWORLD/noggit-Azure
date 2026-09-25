#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
namespace BlizzardArchive::Listfile {
struct FileKey {
  std::string name;
  FileKey(std::string n) : name(std::move(n)) {}
  bool hasFilepath() const { return true; }
  std::string const& filepath() const { return name; }
  int fileDataID() const { return 0; }
  bool operator==(FileKey const& rhs) const { return name == rhs.name; }
};
}
namespace Noggit { enum NoggitRenderContext { TEST_CONTEXT }; }
struct AsyncObject {
  std::atomic<bool> finished{false};
  bool finishedLoading() const { return finished; }
};
struct AsyncLoader {
  static AsyncLoader* instance;
  std::mutex mutex;
  std::condition_variable changed;
  bool deleting = false, release = false;
  std::atomic<int> queued{0};
  void queue_for_load(AsyncObject*) { ++queued; }
  void ensure_deletable(AsyncObject*) {
    std::unique_lock lock(mutex); deleting = true; changed.notify_all();
    changed.wait(lock, [&] { return release; });
  }
};
struct Model : AsyncObject {
  Model(std::string const&, Noggit::NoggitRenderContext) {}
};
