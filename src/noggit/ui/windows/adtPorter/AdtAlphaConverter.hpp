#pragma once

#include <QString>

#include <vector>

namespace Noggit::Ui::Windows::AdtAlphaConverter
{
  enum class Format
  {
    Old,
    Big
  };

  QString formatName(Format format);

  bool readWdtFormat(std::vector<char> const& bytes, Format& format, QString& error);

  bool convert(std::vector<char>& bytes, Format source_format, Format destination_format,
               QString& error);
}
