#include "AdtAlphaConverter.hpp"

#include <noggit/MapHeaders.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace
{
  struct ChunkHeader
  {
    std::uint32_t magic;
    std::uint32_t size;
  };

  struct TopLevelChunk
  {
    std::uint32_t magic = 0;
    std::vector<char> data;
  };

  static_assert(sizeof(ChunkHeader) == 8);
  static_assert(sizeof(MapChunkHeader) == 0x80);
  static_assert(sizeof(ENTRY_MCLY) == 0x10);
  static_assert(sizeof(MHDR) == 0x40);
  static_assert(sizeof(MCIN) == 256 * 0x10);

  template<typename T>
  bool readAt(std::vector<char> const& bytes, std::size_t offset, T& value)
  {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
      return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
  }

  template<typename T>
  bool writeAt(std::vector<char>& bytes, std::size_t offset, T const& value)
  {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
      return false;
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
    return true;
  }

  bool parseTopLevel(std::vector<char> const& bytes, std::vector<TopLevelChunk>& chunks,
                     QString& error)
  {
    chunks.clear();
    std::size_t position = 0;
    while (position + sizeof(ChunkHeader) <= bytes.size())
    {
      ChunkHeader header{};
      std::memcpy(&header, bytes.data() + position, sizeof(header));
      std::size_t const data_position = position + sizeof(ChunkHeader);
      if (header.size > bytes.size() - data_position)
      {
        error = "ADT contains a truncated top-level chunk.";
        return false;
      }
      TopLevelChunk chunk;
      chunk.magic = header.magic;
      chunk.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(data_position),
                        bytes.begin() + static_cast<std::ptrdiff_t>(data_position + header.size));
      chunks.emplace_back(std::move(chunk));
      position = data_position + header.size;
    }
    if (position != bytes.size())
    {
      error = "ADT contains trailing data outside its chunk structure.";
      return false;
    }
    return true;
  }

  std::optional<std::size_t> findChunk(std::vector<TopLevelChunk> const& chunks,
                                       std::uint32_t magic)
  {
    for (std::size_t index = 0; index < chunks.size(); ++index)
      if (chunks[index].magic == magic)
        return index;
    return std::nullopt;
  }

  std::optional<std::size_t> findEitherChunk(std::vector<TopLevelChunk> const& chunks,
                                             std::uint32_t first, std::uint32_t second)
  {
    auto result = findChunk(chunks, first);
    return result ? result : findChunk(chunks, second);
  }

  std::uint8_t roundedDivideBy255(int value)
  {
    return static_cast<std::uint8_t>(value / 255 + (value % 255 <= 127 ? 0 : 1));
  }

  int roundedDivide(int value, int divisor)
  {
    return value / divisor + (value % divisor <= (divisor >> 1) ? 0 : 1);
  }

  bool decodeRle(std::vector<char> const& mcal, std::size_t begin, std::size_t end,
                 std::array<std::uint8_t, 4096>& output, QString& error)
  {
    std::size_t input = begin;
    std::size_t output_position = 0;
    while (output_position < output.size())
    {
      if (input >= end)
      {
        error = "Compressed big-alpha data ends before 4096 pixels were decoded.";
        return false;
      }
      std::uint8_t const control = static_cast<std::uint8_t>(mcal[input++]);
      std::size_t const count = control & 0x7Fu;
      bool const fill = (control & 0x80u) != 0;
      if (!count || count > output.size() - output_position)
      {
        error = "Compressed big-alpha data contains an invalid run length.";
        return false;
      }
      std::size_t const source_size = fill ? 1 : count;
      if (source_size > end - input)
      {
        error = "Compressed big-alpha data contains a truncated run.";
        return false;
      }
      if (fill)
      {
        std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(output_position), count,
                    static_cast<std::uint8_t>(mcal[input]));
      }
      else
      {
        for (std::size_t index = 0; index < count; ++index)
          output[output_position + index] = static_cast<std::uint8_t>(mcal[input + index]);
      }
      input += source_size;
      output_position += count;
    }
    return true;
  }

  bool decodeLayer(std::vector<char> const& mcal, std::size_t begin, std::size_t end,
                   std::uint32_t flags, Noggit::Ui::Windows::AdtAlphaConverter::Format format,
                   bool do_not_fix_alpha_map, std::array<std::uint8_t, 4096>& output,
                   QString& error)
  {
    using Noggit::Ui::Windows::AdtAlphaConverter::Format;
    if (begin > end || end > mcal.size())
    {
      error = "An MCLY alpha offset lies outside MCAL.";
      return false;
    }
    if (format == Format::Big)
    {
      if (flags & FLAG_ALPHA_COMPRESSED)
        return decodeRle(mcal, begin, end, output, error);
      if (end - begin < output.size())
      {
        error = "Uncompressed big-alpha layer is shorter than 4096 bytes.";
        return false;
      }
      std::memcpy(output.data(), mcal.data() + begin, output.size());
      return true;
    }

    if (flags & FLAG_ALPHA_COMPRESSED)
    {
      error = "Old-alpha data unexpectedly uses the big-alpha compression flag.";
      return false;
    }
    if (end - begin < 2048)
    {
      error = "Old-alpha layer is shorter than 2048 bytes.";
      return false;
    }
    for (std::size_t index = 0; index < 2048; ++index)
    {
      std::uint8_t const packed = static_cast<std::uint8_t>(mcal[begin + index]);
      output[index * 2] = static_cast<std::uint8_t>((packed & 0x0Fu) * 17u);
      output[index * 2 + 1] = static_cast<std::uint8_t>(((packed >> 4) & 0x0Fu) * 17u);
    }
    if (!do_not_fix_alpha_map)
    {
      for (std::size_t index = 0; index < 64; ++index)
      {
        output[index * 64 + 63] = output[index * 64 + 62];
        output[63 * 64 + index] = output[62 * 64 + index];
      }
      output[63 * 64 + 63] = output[62 * 64 + 62];
    }
    return true;
  }

  void oldToBig(std::vector<std::array<std::uint8_t, 4096>>& layers)
  {
    for (std::size_t pixel = 0; pixel < 4096; ++pixel)
    {
      int remaining = 255;
      for (std::size_t layer = layers.size(); layer-- > 0;)
      {
        std::uint8_t const value = roundedDivideBy255(layers[layer][pixel] * remaining);
        layers[layer][pixel] = value;
        remaining -= value;
      }
    }
  }

  void bigToOld(std::vector<std::array<std::uint8_t, 4096>>& layers)
  {
    for (std::size_t pixel = 0; pixel < 4096; ++pixel)
    {
      int remaining = 255;
      for (std::size_t layer = layers.size(); layer-- > 0;)
      {
        int const current = layers[layer][pixel];
        if (remaining <= 0)
          layers[layer][pixel] = 0;
        else
          layers[layer][pixel] = static_cast<std::uint8_t>(
            std::clamp(roundedDivide(current * 255, remaining), 0, 255));
        remaining -= current;
      }
    }
  }

  std::vector<char> encodeLayers(
      std::vector<std::array<std::uint8_t, 4096>> const& layers,
      Noggit::Ui::Windows::AdtAlphaConverter::Format format)
  {
    using Noggit::Ui::Windows::AdtAlphaConverter::Format;
    std::vector<char> output;
    std::size_t const layer_size = format == Format::Big ? 4096 : 2048;
    output.reserve(layers.size() * layer_size);
    for (auto const& layer : layers)
    {
      if (format == Format::Big)
      {
        output.insert(output.end(), reinterpret_cast<char const*>(layer.data()),
                      reinterpret_cast<char const*>(layer.data() + layer.size()));
      }
      else
      {
        for (std::size_t index = 0; index < 2048; ++index)
        {
          std::uint8_t const packed = static_cast<std::uint8_t>(
            ((layer[index * 2] & 0xF0u) >> 4) | (layer[index * 2 + 1] & 0xF0u));
          output.push_back(static_cast<char>(packed));
        }
      }
    }
    return output;
  }

  bool adjustOffset(std::uint32_t& offset, std::uint32_t alpha_offset,
                    std::ptrdiff_t delta, QString& error)
  {
    if (!offset || offset <= alpha_offset)
      return true;
    std::int64_t const adjusted = static_cast<std::int64_t>(offset) + delta;
    if (adjusted < 0 || adjusted > std::numeric_limits<std::uint32_t>::max())
    {
      error = "MCNK offset overflow while rebuilding MCAL.";
      return false;
    }
    offset = static_cast<std::uint32_t>(adjusted);
    return true;
  }

  bool convertMcnk(TopLevelChunk& chunk,
                   Noggit::Ui::Windows::AdtAlphaConverter::Format source_format,
                   Noggit::Ui::Windows::AdtAlphaConverter::Format destination_format,
                   QString& error)
  {
    using Noggit::Ui::Windows::AdtAlphaConverter::Format;
    MapChunkHeader header{};
    if (!readAt(chunk.data, 0, header))
    {
      error = "MCNK is shorter than its 128-byte header.";
      return false;
    }
    if (header.nLayers > 4)
    {
      error = QString("MCNK %1,%2 declares more than four texture layers.")
        .arg(header.ix).arg(header.iy);
      return false;
    }
    if (header.nLayers <= 1)
      return true;
    if (header.ofsLayer < 8 || header.ofsAlpha < 8)
    {
      error = QString("MCNK %1,%2 has invalid MCLY or MCAL offsets.")
        .arg(header.ix).arg(header.iy);
      return false;
    }

    std::size_t const mcly_position = header.ofsLayer - 8;
    std::size_t const mcal_position = header.ofsAlpha - 8;
    ChunkHeader mcly_header{};
    ChunkHeader mcal_header{};
    if (!readAt(chunk.data, mcly_position, mcly_header) || mcly_header.magic != 'MCLY'
        || !readAt(chunk.data, mcal_position, mcal_header) || mcal_header.magic != 'MCAL')
    {
      error = QString("MCNK %1,%2 does not contain MCLY/MCAL at its recorded offsets.")
        .arg(header.ix).arg(header.iy);
      return false;
    }
    std::size_t const required_mcly = static_cast<std::size_t>(header.nLayers) * sizeof(ENTRY_MCLY);
    if (mcly_header.size < required_mcly
        || mcly_position + sizeof(ChunkHeader) > chunk.data.size()
        || required_mcly > chunk.data.size() - mcly_position - sizeof(ChunkHeader)
        || mcal_position + sizeof(ChunkHeader) > chunk.data.size()
        || mcal_header.size > chunk.data.size() - mcal_position - sizeof(ChunkHeader))
    {
      error = QString("MCNK %1,%2 contains truncated texture chunks.")
        .arg(header.ix).arg(header.iy);
      return false;
    }

    std::vector<ENTRY_MCLY> entries(header.nLayers);
    std::memcpy(entries.data(), chunk.data.data() + mcly_position + sizeof(ChunkHeader),
                required_mcly);
    std::vector<char> const mcal(
      chunk.data.begin() + static_cast<std::ptrdiff_t>(mcal_position + sizeof(ChunkHeader)),
      chunk.data.begin() + static_cast<std::ptrdiff_t>(mcal_position + sizeof(ChunkHeader)
                                                       + mcal_header.size));
    std::vector<std::array<std::uint8_t, 4096>> layers(header.nLayers - 1);
    for (std::size_t layer = 1; layer < entries.size(); ++layer)
    {
      if (!(entries[layer].flags & FLAG_USE_ALPHA))
      {
        error = QString("MCNK %1,%2 texture layer %3 has no alpha-map flag.")
          .arg(header.ix).arg(header.iy).arg(layer);
        return false;
      }
      std::size_t const begin = entries[layer].ofsAlpha;
      std::size_t const end = layer + 1 < entries.size()
        ? static_cast<std::size_t>(entries[layer + 1].ofsAlpha) : mcal.size();
      if (!decodeLayer(mcal, begin, end, entries[layer].flags, source_format,
                       !!header.flags.flags.do_not_fix_alpha_map, layers[layer - 1], error))
      {
        error = QString("MCNK %1,%2 layer %3: %4")
          .arg(header.ix).arg(header.iy).arg(layer).arg(error);
        return false;
      }
    }

    if (source_format == Format::Old && destination_format == Format::Big)
      oldToBig(layers);
    else if (source_format == Format::Big && destination_format == Format::Old)
      bigToOld(layers);

    std::vector<char> const encoded = encodeLayers(layers, destination_format);
    std::size_t const destination_layer_size = destination_format == Format::Big ? 4096 : 2048;
    entries[0].flags &= ~(FLAG_USE_ALPHA | FLAG_ALPHA_COMPRESSED);
    entries[0].ofsAlpha = 0;
    for (std::size_t layer = 1; layer < entries.size(); ++layer)
    {
      entries[layer].flags |= FLAG_USE_ALPHA;
      entries[layer].flags &= ~FLAG_ALPHA_COMPRESSED;
      entries[layer].ofsAlpha = static_cast<std::uint32_t>((layer - 1) * destination_layer_size);
    }
    std::memcpy(chunk.data.data() + mcly_position + sizeof(ChunkHeader), entries.data(),
                required_mcly);

    std::vector<char> rebuilt;
    rebuilt.reserve(chunk.data.size() - mcal_header.size + encoded.size());
    rebuilt.insert(rebuilt.end(), chunk.data.begin(),
                   chunk.data.begin() + static_cast<std::ptrdiff_t>(mcal_position));
    ChunkHeader const new_mcal_header{'MCAL', static_cast<std::uint32_t>(encoded.size())};
    rebuilt.insert(rebuilt.end(), reinterpret_cast<char const*>(&new_mcal_header),
                   reinterpret_cast<char const*>(&new_mcal_header) + sizeof(new_mcal_header));
    rebuilt.insert(rebuilt.end(), encoded.begin(), encoded.end());
    std::size_t const old_mcal_end = mcal_position + sizeof(ChunkHeader) + mcal_header.size;
    rebuilt.insert(rebuilt.end(), chunk.data.begin() + static_cast<std::ptrdiff_t>(old_mcal_end),
                   chunk.data.end());

    std::ptrdiff_t const delta = static_cast<std::ptrdiff_t>(encoded.size())
                               - static_cast<std::ptrdiff_t>(mcal_header.size);
    header.sizeAlpha = static_cast<std::uint32_t>(sizeof(ChunkHeader) + encoded.size());
    if (!adjustOffset(header.ofsHeight, header.ofsAlpha, delta, error)
        || !adjustOffset(header.ofsNormal, header.ofsAlpha, delta, error)
        || !adjustOffset(header.ofsLayer, header.ofsAlpha, delta, error)
        || !adjustOffset(header.ofsRefs, header.ofsAlpha, delta, error)
        || !adjustOffset(header.ofsShadow, header.ofsAlpha, delta, error)
        || !adjustOffset(header.ofsSndEmitters, header.ofsAlpha, delta, error)
        || !adjustOffset(header.ofsLiquid, header.ofsAlpha, delta, error)
        || !adjustOffset(header.ofsMCCV, header.ofsAlpha, delta, error))
      return false;
    if (!writeAt(rebuilt, 0, header))
    {
      error = "Unable to update the rebuilt MCNK header.";
      return false;
    }
    chunk.data = std::move(rebuilt);
    return true;
  }

  std::vector<std::size_t> calculatePositions(std::vector<TopLevelChunk> const& chunks)
  {
    std::vector<std::size_t> positions;
    positions.reserve(chunks.size());
    std::size_t position = 0;
    for (TopLevelChunk const& chunk : chunks)
    {
      positions.push_back(position);
      position += sizeof(ChunkHeader) + chunk.data.size();
    }
    return positions;
  }

  bool updateMcin(std::vector<TopLevelChunk>& chunks,
                  std::vector<std::size_t> const& positions, QString& error)
  {
    auto const mcin_index = findChunk(chunks, 'MCIN');
    if (!mcin_index || chunks[*mcin_index].data.size() < sizeof(MCIN))
    {
      error = "ADT does not contain a complete MCIN table.";
      return false;
    }
    MCIN mcin{};
    std::memcpy(&mcin, chunks[*mcin_index].data.data(), sizeof(mcin));
    std::array<bool, 256> found{};
    std::size_t mcnk_count = 0;
    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
      if (chunks[index].magic != 'MCNK')
        continue;
      MapChunkHeader header{};
      if (!readAt(chunks[index].data, 0, header) || header.ix >= 16 || header.iy >= 16)
      {
        error = "ADT contains an MCNK with invalid chunk coordinates.";
        return false;
      }
      std::size_t const entry_index = header.iy * 16 + header.ix;
      if (found[entry_index])
      {
        error = "ADT contains duplicate MCNK chunk coordinates.";
        return false;
      }
      if (positions[index] > std::numeric_limits<std::uint32_t>::max()
          || chunks[index].data.size() + sizeof(ChunkHeader)
             > std::numeric_limits<std::uint32_t>::max())
      {
        error = "ADT grew beyond the WotLK 32-bit offset range.";
        return false;
      }
      found[entry_index] = true;
      mcin.mEntries[entry_index].offset = static_cast<std::uint32_t>(positions[index]);
      mcin.mEntries[entry_index].size = static_cast<std::uint32_t>(
        chunks[index].data.size() + sizeof(ChunkHeader));
      ++mcnk_count;
    }
    if (mcnk_count != 256 || std::find(found.begin(), found.end(), false) != found.end())
    {
      error = QString("ADT contains %1 uniquely positioned MCNK chunks instead of 256.")
        .arg(mcnk_count);
      return false;
    }
    std::memcpy(chunks[*mcin_index].data.data(), &mcin, sizeof(mcin));
    return true;
  }

  bool updateMhdr(std::vector<TopLevelChunk>& chunks,
                  std::vector<std::size_t> const& positions, QString& error)
  {
    auto const mhdr_index = findChunk(chunks, 'MHDR');
    if (!mhdr_index || chunks[*mhdr_index].data.size() < sizeof(MHDR))
    {
      error = "ADT does not contain a complete MHDR chunk.";
      return false;
    }
    MHDR mhdr{};
    std::memcpy(&mhdr, chunks[*mhdr_index].data.data(), sizeof(mhdr));
    std::size_t const base = positions[*mhdr_index] + sizeof(ChunkHeader);
    auto set_offset = [&](std::uint32_t& field, std::optional<std::size_t> index) -> bool
    {
      if (!index)
      {
        field = 0;
        return true;
      }
      if (positions[*index] < base
          || positions[*index] - base > std::numeric_limits<std::uint32_t>::max())
      {
        error = "MHDR offset overflow while rebuilding the ADT.";
        return false;
      }
      field = static_cast<std::uint32_t>(positions[*index] - base);
      return true;
    };
    if (!set_offset(mhdr.mcin, findChunk(chunks, 'MCIN'))
        || !set_offset(mhdr.mtex, findChunk(chunks, 'MTEX'))
        || !set_offset(mhdr.mmdx, findChunk(chunks, 'MMDX'))
        || !set_offset(mhdr.mmid, findChunk(chunks, 'MMID'))
        || !set_offset(mhdr.mwmo, findChunk(chunks, 'MWMO'))
        || !set_offset(mhdr.mwid, findChunk(chunks, 'MWID'))
        || !set_offset(mhdr.mddf, findChunk(chunks, 'MDDF'))
        || !set_offset(mhdr.modf, findChunk(chunks, 'MODF'))
        || !set_offset(mhdr.mfbo, findChunk(chunks, 'MFBO'))
        || !set_offset(mhdr.mh2o, findChunk(chunks, 'MH2O'))
        || !set_offset(mhdr.mtxf, findEitherChunk(chunks, 'MTXF', 'MTFX')))
      return false;
    std::memcpy(chunks[*mhdr_index].data.data(), &mhdr, sizeof(mhdr));
    return true;
  }

  std::vector<char> serialize(std::vector<TopLevelChunk> const& chunks)
  {
    std::size_t total_size = 0;
    for (TopLevelChunk const& chunk : chunks)
      total_size += sizeof(ChunkHeader) + chunk.data.size();
    std::vector<char> output;
    output.reserve(total_size);
    for (TopLevelChunk const& chunk : chunks)
    {
      ChunkHeader const header{chunk.magic, static_cast<std::uint32_t>(chunk.data.size())};
      output.insert(output.end(), reinterpret_cast<char const*>(&header),
                    reinterpret_cast<char const*>(&header) + sizeof(header));
      output.insert(output.end(), chunk.data.begin(), chunk.data.end());
    }
    return output;
  }

  bool validateConverted(std::vector<TopLevelChunk> const& chunks,
                         std::vector<std::size_t> const& positions,
                         Noggit::Ui::Windows::AdtAlphaConverter::Format format,
                         QString& error)
  {
    using Noggit::Ui::Windows::AdtAlphaConverter::Format;
    auto const mcin_index = findChunk(chunks, 'MCIN');
    if (!mcin_index || chunks[*mcin_index].data.size() < sizeof(MCIN))
    {
      error = "Converted ADT is missing MCIN.";
      return false;
    }
    MCIN mcin{};
    std::memcpy(&mcin, chunks[*mcin_index].data.data(), sizeof(mcin));
    std::size_t const expected_layer_size = format == Format::Big ? 4096 : 2048;
    std::size_t mcnk_count = 0;
    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
      if (chunks[index].magic != 'MCNK')
        continue;
      MapChunkHeader header{};
      if (!readAt(chunks[index].data, 0, header) || header.ix >= 16 || header.iy >= 16)
      {
        error = "Converted ADT contains an invalid MCNK header.";
        return false;
      }
      ENTRY_MCIN const& entry = mcin.mEntries[header.iy * 16 + header.ix];
      if (entry.offset != positions[index]
          || entry.size != chunks[index].data.size() + sizeof(ChunkHeader))
      {
        error = QString("Converted ADT has a stale MCIN entry for MCNK %1,%2.")
          .arg(header.ix).arg(header.iy);
        return false;
      }
      if (header.nLayers > 1)
      {
        if (header.ofsLayer < 8 || header.ofsAlpha < 8)
        {
          error = "Converted MCNK has invalid texture offsets.";
          return false;
        }
        std::size_t const mcly_position = header.ofsLayer - 8;
        std::size_t const mcal_position = header.ofsAlpha - 8;
        ChunkHeader mcly{};
        ChunkHeader mcal{};
        if (!readAt(chunks[index].data, mcly_position, mcly) || mcly.magic != 'MCLY'
            || !readAt(chunks[index].data, mcal_position, mcal) || mcal.magic != 'MCAL'
            || mcal.size != (header.nLayers - 1) * expected_layer_size
            || header.sizeAlpha != mcal.size + sizeof(ChunkHeader))
        {
          error = QString("Converted MCNK %1,%2 has inconsistent MCLY/MCAL sizes.")
            .arg(header.ix).arg(header.iy);
          return false;
        }
        for (std::size_t layer = 1; layer < header.nLayers; ++layer)
        {
          ENTRY_MCLY entry_layer{};
          if (!readAt(chunks[index].data,
                      mcly_position + sizeof(ChunkHeader) + layer * sizeof(ENTRY_MCLY),
                      entry_layer)
              || !(entry_layer.flags & FLAG_USE_ALPHA)
              || (entry_layer.flags & FLAG_ALPHA_COMPRESSED)
              || entry_layer.ofsAlpha != (layer - 1) * expected_layer_size)
          {
            error = QString("Converted MCNK %1,%2 has inconsistent MCLY layer %3.")
              .arg(header.ix).arg(header.iy).arg(layer);
            return false;
          }
        }
      }
      ++mcnk_count;
    }
    if (mcnk_count != 256)
    {
      error = QString("Converted ADT contains %1 MCNK chunks instead of 256.").arg(mcnk_count);
      return false;
    }
    return true;
  }
}

namespace Noggit::Ui::Windows::AdtAlphaConverter
{
  QString formatName(Format format)
  {
    return format == Format::Big ? "Big Alpha (8-bit)" : "Old Alpha (4-bit)";
  }

  bool readWdtFormat(std::vector<char> const& bytes, Format& format, QString& error)
  {
    std::size_t position = 0;
    bool valid_version = false;
    bool found_mphd = false;
    while (position + sizeof(ChunkHeader) <= bytes.size())
    {
      ChunkHeader header{};
      std::memcpy(&header, bytes.data() + position, sizeof(header));
      std::size_t const data_position = position + sizeof(ChunkHeader);
      if (header.size > bytes.size() - data_position)
      {
        error = "WDT contains a truncated top-level chunk.";
        return false;
      }
      if (header.magic == 'MVER' && header.size == sizeof(std::uint32_t))
      {
        std::uint32_t version = 0;
        std::memcpy(&version, bytes.data() + data_position, sizeof(version));
        valid_version = version == 18;
      }
      else if (header.magic == 'MPHD' && header.size >= sizeof(std::uint32_t))
      {
        std::uint32_t flags = 0;
        std::memcpy(&flags, bytes.data() + data_position, sizeof(flags));
        format = flags & FLAG_BIG_ALPHA ? Format::Big : Format::Old;
        found_mphd = true;
      }
      position = data_position + header.size;
    }
    if (position != bytes.size())
    {
      error = "WDT contains trailing data outside its chunk structure.";
      return false;
    }
    if (!valid_version)
    {
      error = "Only WotLK WDT version 18 is supported.";
      return false;
    }
    if (!found_mphd)
    {
      error = "WDT does not contain a valid MPHD map header.";
      return false;
    }
    return true;
  }

  bool convert(std::vector<char>& bytes, Format source_format, Format destination_format,
               QString& error)
  {
    if (source_format == destination_format)
      return true;

    std::vector<TopLevelChunk> chunks;
    if (!parseTopLevel(bytes, chunks, error))
      return false;
    auto const version_index = findChunk(chunks, 'MVER');
    std::uint32_t version = 0;
    if (!version_index || chunks[*version_index].data.size() != sizeof(version)
        || !readAt(chunks[*version_index].data, 0, version) || version != 18)
    {
      error = "Only WotLK ADT version 18 is supported for alpha conversion.";
      return false;
    }

    std::size_t mcnk_count = 0;
    for (TopLevelChunk& chunk : chunks)
    {
      if (chunk.magic != 'MCNK')
        continue;
      if (!convertMcnk(chunk, source_format, destination_format, error))
        return false;
      ++mcnk_count;
    }
    if (mcnk_count != 256)
    {
      error = QString("Source ADT contains %1 MCNK chunks instead of 256.").arg(mcnk_count);
      return false;
    }

    std::vector<std::size_t> const positions = calculatePositions(chunks);
    if (!updateMcin(chunks, positions, error)
        || !updateMhdr(chunks, positions, error)
        || !validateConverted(chunks, positions, destination_format, error))
      return false;

    bytes = serialize(chunks);
    return true;
  }
}
