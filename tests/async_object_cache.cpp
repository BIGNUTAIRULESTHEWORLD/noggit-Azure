// Exercises the production cache template with a controllable loader.
#include <noggit/AsyncObjectMultimap.hpp>
#include <future>
#include <iostream>
AsyncLoader* AsyncLoader::instance;
int main() {
  for (bool finished : {false, true}) {
  AsyncLoader loader; AsyncLoader::instance = &loader;
  Noggit::AsyncObjectMultimap<Model> cache;
  BlizzardArchive::Listfile::FileKey key("grass.m2");
  auto* original = cache.emplace(key, Noggit::TEST_CONTEXT);
  original->finished = finished;
  auto retiring = std::async(std::launch::async, [&] { cache.erase(key, Noggit::TEST_CONTEXT); });
  { std::unique_lock lock(loader.mutex); loader.changed.wait(lock, [&] { return loader.deleting; }); }
  std::promise<void> started;
  auto replacing = std::async(std::launch::async, [&] {
    started.set_value(); return cache.emplace(key, Noggit::TEST_CONTEXT);
  });
  started.get_future().wait();
  auto* replacement = replacing.get();
  bool const separate = replacement != original;
  { std::lock_guard lock(loader.mutex); loader.release = true; loader.changed.notify_all(); }
  retiring.get();
  if (!separate) { std::cerr << "FAIL: cache returned a model while its final release was pending\n"; return 1; }
  if (loader.queued != 2) return 2;
  // The replacement must remain alive and share subsequent references.
  auto* a = cache.emplace(key, Noggit::TEST_CONTEXT);
  auto* b = cache.emplace(key, Noggit::TEST_CONTEXT);
  if (a != b || loader.queued != 2) return 3;
  cache.erase(key, Noggit::TEST_CONTEXT);
  cache.erase(key, Noggit::TEST_CONTEXT);
  cache.erase(key, Noggit::TEST_CONTEXT);
  }
  std::cout << "Async cache: final-release/reload interleaving and replacement lifetime passed.\n";
}
