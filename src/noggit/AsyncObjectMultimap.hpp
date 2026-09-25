// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/AsyncLoader.h>
#include <noggit/AsyncObject.h>
#include <noggit/ContextObject.hpp>
#include <noggit/Model.h>

#include <Listfile.hpp>

#include <functional>
#include <string>
#include <unordered_map>

struct pair_hash
{
  std::size_t operator() (const std::pair<int, BlizzardArchive::Listfile::FileKey> &p) const noexcept
  {
    auto h1 = std::hash<int>{}(p.first);
    auto h2 = std::hash<std::string>{}(p.second.hasFilepath() ? p.second.filepath() : "");
    auto h3 = std::hash<int>{}(p.second.fileDataID());

    return h1 ^ h2 ^ h3;
  }
};

namespace Noggit
{

  template<typename T>
  struct AsyncObjectMultimap
  {
    AsyncObjectMultimap() = default;
    ~AsyncObjectMultimap()
    {
      /*
      apply ( [&] (std::string const& key, T const&)
              {
                auto pair = std::make_pair(context, key);
                LogDebug << key << ": " << _counts.at(pair) << std::endl;
              }
            );
      */
    }

    template<typename... Args>
      T* emplace (BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context, Args&&... args)
    {
      std::scoped_lock const lock(_mutex);
      auto pair = std::make_pair(context, file_key);
      //LogDebug << "Emplacing " << normalized << " into context" << context << std::endl;

      {
        if ([&] { return _counts[pair]++; }())
        {
          return &_elements.at (pair);
        }
      }

      T* const obj ( [&]
                     {
                       return &_elements.emplace ( std::piecewise_construct
                                                 , std::forward_as_tuple (pair)
                                                 , std::forward_as_tuple (file_key.filepath(), context, args...)
                                                 ).first->second;
                     }()
                   );

      AsyncLoader::instance->queue_for_load(static_cast<AsyncObject*>(obj));

      return obj; 
    }
    void erase (BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
    {
      auto pair = std::make_pair(context, file_key);
      //LogDebug << "Erasing " << normalized << " from context" << context << std::endl;

      typename decltype(_elements)::node_type retired;

      {
        std::scoped_lock lock(_mutex);

        if (--_counts.at(pair) == 0)
        {
          // Detach atomically with the last reference. A concurrent emplace
          // must create a new object, never resurrect one pending deletion.
          retired = _elements.extract(pair);
          _counts.erase(pair);
        }
      }

      if (!retired.empty())
      {
        // The node owns the old object while the loader finishes. Even when
        // finishedLoading is true, the worker may still be returning/logging.
        // Wait and destroy outside the cache lock, leaving any replacement intact.
        AsyncLoader::instance->ensure_deletable(static_cast<AsyncObject*>(&retired.mapped()));
      }
    }
    void apply (std::function<void (BlizzardArchive::Listfile::FileKey const&, T&)> fun)
    {
      std::scoped_lock lock(_mutex);

      for (auto& element : _elements)
      {
        fun (element.first.second, element.second);
      }
    }
    void apply (std::function<void (BlizzardArchive::Listfile::FileKey const&, T const&)> fun) const
    {
      std::scoped_lock lock(_mutex);
      for (auto const& element : _elements)
      {
        fun (element.first.second, element.second);
      }
    }

    void context_aware_apply(std::function<void (BlizzardArchive::Listfile::FileKey const&, T&)> fun, Noggit::NoggitRenderContext context)
    {
      std::scoped_lock lock(_mutex);

      for (auto& element : _elements)
      {
        if (element.first.first != context)
          continue;

        fun (element.first.second, element.second);
      }
    }
    void context_aware_apply(std::function<void (BlizzardArchive::Listfile::FileKey const&, T const&)> fun, Noggit::NoggitRenderContext context) const
    {
      std::scoped_lock lock(_mutex);
      for (auto const& element : _elements)
      {
        if (element.first.first != context)
          continue;

        fun (element.first.second, element.second);
      }
    }

  private:
    std::unordered_map<std::pair<int, BlizzardArchive::Listfile::FileKey>, T, pair_hash> _elements;
    std::unordered_map<std::pair<int, BlizzardArchive::Listfile::FileKey>, std::size_t, pair_hash> _counts;
    std::mutex mutable _mutex;
  };

}
