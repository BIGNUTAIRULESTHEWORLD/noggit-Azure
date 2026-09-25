#pragma once

#include <QSettings>
#include <QString>
#include <QWidget>

namespace Noggit::Ui
{
  inline bool azureColorMode()
  {
    return QSettings().value(QStringLiteral("ui/color_mode"), QStringLiteral("Azure")).toString()
        != QStringLiteral("Dark");
  }

  inline void setAzureColorMode(bool azure)
  {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/color_mode"),
                      azure ? QStringLiteral("Azure") : QStringLiteral("Dark"));
    settings.sync();
  }

  // Azure-specific UI panels have their own palette. Match those colors to
  // the bundled Dark theme without changing their layout or base theme.
  inline QString colorModeStyle(QString style, bool azure = azureColorMode())
  {
    if (azure)
      return style;

    static constexpr struct { const char* azure; const char* dark; } colors[] = {
      {"#0b1220", "#26282d"}, {"#0b1426", "#26282d"},
      {"#0a1326", "#26282d"}, {"#0b1628", "#1f2023"},
      {"#080e1b", "#1f2023"}, {"#0b1729", "#1f2023"},
      {"#101b31", "#26282d"}, {"#101d35", "#26282d"},
      {"#0d182b", "#1f2023"}, {"#0d1a30", "#1f2023"},
      {"#0c192d", "#1f2023"}, {"#111d30", "#26282d"},
      {"#14233e", "#2d2f34"}, {"#12213a", "#2d2f34"},
      {"#111b2d", "#1f2023"}, {"#1a2b45", "#2d2f34"},
      {"#172944", "#2d2f34"}, {"#16263c", "#373b40"},
      {"#182943", "#2d2f34"}, {"#223958", "#373b40"},
      {"#253a55", "#2d2f34"},
      {"#19283d", "#2d2f34"}, {"#213b5c", "#373b40"},
      {"#14233a", "#2d2f34"}, {"#142c49", "#2d2f34"},
      {"#1a2450", "#2d2f34"}, {"#1c293c", "#2d2f34"},
      {"#1c2b4c", "#2d2f34"}, {"#203c5d", "#373b40"},
      {"#224367", "#373b40"}, {"#253a60", "#373b40"},
      {"#2c3d55", "#373b40"}, {"#30557d", "#45494f"},
      {"#4b4070", "#373b40"},
      {"#263c61", "#373b40"}, {"#294565", "#373b40"},
      {"#315f96", "#5281b9"}, {"#173358", "#2d2f34"},
      {"#365581", "#45494f"}, {"#526783", "#45494f"},
      {"#50647e", "#45494f"}, {"#64738c", "#45494f"},
      {"#75858b", "#45494f"},
      {"#3d5271", "#45494f"}, {"#405c7c", "#45494f"},
      {"#d2b474", "#5281b9"}, {"#d6b777", "#5281b9"},
      {"#a88f5f", "#5281b9"}, {"#8ec3e6", "#d7d7d7"},
      {"#6fa9df", "#d7d7d7"}, {"#b0d9ef", "#d7d7d7"},
      {"#9cb0c5", "#a9a9a9"}, {"#7e8b9c", "#7f7f7f"},
      {"#b6c4d2", "#d7d7d7"}, {"#d1dbe5", "#d7d7d7"},
      {"#aabbd0", "#a9a9a9"}, {"#ece8df", "#d7d7d7"},
      {"#eee8da", "#d7d7d7"}, {"#e9edf4", "#d7d7d7"},
      {"#f0dcaf", "#d7d7d7"}, {"#f1eee7", "#d7d7d7"},
      {"#f6e3b5", "#d7d7d7"}, {"#f4dfb1", "#d7d7d7"}
    };
    for (auto const& color : colors)
      style.replace(QLatin1String(color.azure), QLatin1String(color.dark), Qt::CaseInsensitive);
    return style;
  }

  inline QString projectColorStyle(QString style)
  {
    return colorModeStyle(style);
  }

  inline void setColorModeStyle(QWidget* widget, QString const& azureStyle)
  {
    widget->setProperty("azureStyledStyle", azureStyle);
    widget->setProperty("darkStyledStyle", colorModeStyle(azureStyle, false));
    widget->setStyleSheet(azureColorMode() ? azureStyle
                                           : widget->property("darkStyledStyle").toString());
  }
}
