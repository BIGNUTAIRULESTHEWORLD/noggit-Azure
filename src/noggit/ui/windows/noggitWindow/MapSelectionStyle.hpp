#pragma once

#include <QString>

namespace Noggit::Ui::Windows
{
  inline QString mapSelectionStyle()
  {
    return QStringLiteral(R"QSS(
      QWidget#mapSelectionPage, QWidget#mapSelectionContent {
        background: #0b1220;
        color: #ece8df;
      }
      QFrame#mapSelectionHeader {
        background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                    stop:0 #1a2b45, stop:1 #111b2d);
        border: none;
        border-bottom: 1px solid #d2b474;
      }
      QLabel#mapBrandMark {
        color: #8ec3e6;
        background: #12213a;
        border: 1px solid #d2b474;
        font-family: Georgia;
        font-size: 25px;
      }
      QLabel#mapBrandName {
        color: #ece8df;
        font-family: Georgia;
        font-size: 19px;
      }
      QLabel#mapBrandSection {
        color: #d2b474;
        font-family: Segoe UI;
        font-size: 9px;
      }
      QLabel#mapProjectName {
        color: #9cb0c5;
        font-family: Segoe UI;
        font-size: 12px;
      }
      QPushButton#mapHeaderSettings {
        color: #9cb0c5;
        background: transparent;
        border: none;
        padding: 8px;
        font-family: Segoe UI;
        font-size: 12px;
      }
      QPushButton#mapHeaderSettings:hover { color: #d2b474; }
      QTabWidget#mapBrowserTabs {
        background: #111d30;
        border-right: 1px solid #3d5271;
      }
      QTabWidget#mapBrowserTabs::pane {
        background: #111d30;
        border: none;
      }
      QTabWidget#mapBrowserTabs QTabBar::tab {
        color: #9cb0c5;
        background: #111d30;
        border: none;
        border-bottom: 2px solid transparent;
        min-width: 110px;
        padding: 9px 12px;
      }
      QTabWidget#mapBrowserTabs QTabBar::tab:selected {
        color: #ece8df;
        border-bottom-color: #8ec3e6;
      }
      QWidget#mapsBrowserPage { background: #111d30; }
      QLineEdit#mapSearchInput, QComboBox#mapTypeFilter,
      QComboBox#mapExpansionFilter {
        color: #ece8df;
        background: #14233a;
        border: 1px solid #3d5271;
        padding: 5px 7px;
        min-height: 23px;
        font-family: Segoe UI;
        font-size: 11px;
      }
      QLineEdit#mapSearchInput { background: #0b1628; }
      QLabel#mapFilterLabel, QLabel#mapListHeading {
        color: #d2b474;
        font-family: Segoe UI;
        font-size: 10px;
      }
      QLabel#mapListHeading {
        border-bottom: 1px solid #3d5271;
        padding-bottom: 6px;
      }
      QLabel#mapListCount {
        color: #9cb0c5;
        font-family: Segoe UI;
        font-size: 10px;
      }
      QCheckBox#mapWmoFilter, QCheckBox#mapGridToggle {
        color: #b6c4d2;
        font-family: Segoe UI;
        font-size: 11px;
      }
      QListWidget#mapList, QListWidget#bookmarksList {
        background: transparent;
        border: none;
        outline: none;
      }
      QListWidget#mapList::item, QListWidget#bookmarksList::item {
        background: transparent;
        border: 1px solid transparent;
        border-bottom-color: #2c3d55;
      }
      QListWidget#mapList::item:hover, QListWidget#bookmarksList::item:hover,
      QListWidget#mapList::item:selected, QListWidget#bookmarksList::item:selected {
        background: #213b5c;
        border-color: #a88f5f;
      }
      QLabel#mapListName {
        color: #ece8df;
        background: transparent;
        font-family: Segoe UI;
        font-size: 12px;
      }
      QLabel#mapListMeta {
        color: #9cb0c5;
        background: transparent;
        font-family: Segoe UI;
        font-size: 10px;
      }
      QPushButton#addMapButton {
        color: #f0dcaf;
        background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                    stop:0 #224367, stop:1 #142c49);
        border: 1px solid #d2b474;
        min-height: 36px;
        font-family: Segoe UI;
        font-size: 12px;
      }
      QPushButton#addMapButton:hover { background: #30557d; }
      QTabWidget#mapModeTabs::pane {
        background: #0b1220;
        border: none;
      }
      QTabWidget#mapModeTabs QTabBar::tab {
        color: #9cb0c5;
        background: #0b1220;
        border: none;
        border-top: 2px solid transparent;
        padding: 11px 18px;
      }
      QTabWidget#mapModeTabs QTabBar::tab:selected {
        color: #ece8df;
        background: #16263c;
        border-top-color: #8ec3e6;
      }
      QWidget#enterMapPage { background: #0b1220; }
      QPushButton#mapAppearanceButton {
        color: #ece8df;
        background: #19283d;
        border: 1px solid #3d5271;
        padding: 7px 9px;
        font-family: Segoe UI;
        font-size: 11px;
      }
      QPushButton#mapAppearanceButton:hover { border-color: #d2b474; }
      QFrame#mapEmptyPreview {
        background: qlineargradient(x1:0, y1:0, x2:0.85, y2:1,
                                    stop:0 #1a2450, stop:0.46 #4b4070,
                                    stop:0.76 #203c5d, stop:1 #0b1729);
        border: 1px solid #50647e;
      }
      QLabel#mapEmptyKicker, QLabel#mapPreviewKicker {
        color: #d2b474;
        font-family: Segoe UI;
        font-size: 10px;
      }
      QLabel#mapEmptyTitle {
        color: #f1eee7;
        font-family: Georgia;
        font-size: 34px;
      }
      QLabel#mapEmptyHint {
        color: #d1dbe5;
        font-family: Segoe UI;
        font-size: 13px;
      }
      QFrame#mapSelectedPreview {
        background: #0d1a30;
        border: 1px solid #50647e;
      }
      QLabel#mapPreviewName {
        color: #f1eee7;
        font-family: Georgia;
        font-size: 24px;
      }
      QLabel#mapPreviewId, QLabel#mapPreviewHint {
        color: #9cb0c5;
        font-family: Segoe UI;
        font-size: 11px;
      }
      QScrollArea#mapMinimapHolder {
        background: #0d182b;
        border: 1px solid #75858b;
      }
      QScrollBar:vertical { background: #0b1628; width: 10px; }
      QScrollBar::handle:vertical { background: #526783; min-height: 26px; }
      QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
    )QSS");
  }
}
