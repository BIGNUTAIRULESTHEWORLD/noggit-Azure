// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ChunkTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/rendering/WorldRender.hpp>
#include <noggit/ui/tools/ChunkManipulator/ChunkClipboard.hpp>
#include <noggit/ui/tools/ChunkManipulator/ChunkManipulatorPanel.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QWheelEvent>

namespace Noggit
{
    using namespace Ui::Tools::ChunkManipulator;

    ChunkTool::ChunkTool(MapView* mapView)
        : Tool{mapView}
    {
        addHotkey("chunkMoverPaste"_hash, {
            .onPress = [this] { pasteAtCursor(); },
            .condition = [this] { return _clipboard && _clipboard->hasCopy(); }
        });
        addHotkey("chunkMoverClear"_hash, {
            .onPress = [this] { clearSelection(); },
            .condition = [this]
            {
                return _clipboard && (_clipboard->hasCopy() || !_clipboard->selectedChunks().empty()
                    || (_chunkManipulator && _chunkManipulator->hasAssetSelection()));
            }
        });
        addHotkey("chunkMoverRotate"_hash, {
            .onPress = [this] { if (_chunkManipulator) _chunkManipulator->rotateClockwise(); },
            .condition = [this] { return _clipboard && _clipboard->hasCopy(); }
        });
        addHotkey("chunkMoverMirrorHorizontal"_hash, {
            .onPress = [this] { if (_chunkManipulator) _chunkManipulator->toggleMirrorHorizontal(); },
            .condition = [this] { return _clipboard && _clipboard->hasCopy(); }
        });
        addHotkey("chunkMoverMirrorVertical"_hash, {
            .onPress = [this] { if (_chunkManipulator) _chunkManipulator->toggleMirrorVertical(); },
            .condition = [this] { return _clipboard && _clipboard->hasCopy(); }
        });
    }

    ChunkTool::~ChunkTool()
    {
        delete _clipboard;
        _clipboard = nullptr;
        delete _chunkManipulator;
    }

    char const* ChunkTool::name() const
    {
        return "Chunk Mover";
    }

    editing_mode ChunkTool::editingMode() const
    {
        return editing_mode::chunk;
    }

    Ui::FontNoggit::Icons ChunkTool::icon() const
    {
        // A cube reads as a terrain chunk and is distinct from the scripting
        // tool's information glyph.
        return Ui::FontNoggit::CUBE;
    }

    void ChunkTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _clipboard = new ChunkClipboard(mapView(), mapView());
        _chunkManipulator = new ChunkManipulatorPanel(mapView());
        toolPanel->registerTool(this, _chunkManipulator);

        QObject::connect(_clipboard, &ChunkClipboard::selectionChanged, _chunkManipulator,
            [this](auto const& selection) { _chunkManipulator->setSelectionCount(selection.size()); });
        QObject::connect(_clipboard, &ChunkClipboard::copied, _chunkManipulator,
            [this](std::size_t count, std::size_t m2s, std::size_t wmos)
            { _chunkManipulator->setClipboardCount(count, m2s, wmos); });
        QObject::connect(_clipboard, &ChunkClipboard::pasted, _chunkManipulator,
            [this](ChunkPasteResult result) { _chunkManipulator->showPasteResult(result); });
        QObject::connect(NOGGIT_ACTION_MGR, &ActionManager::historyNavigated, _clipboard,
            [this]
            {
                _clipboard->clearPreviewForHistoryChange(mapView()->cursorPosition());
            });
        QObject::connect(_chunkManipulator, &ChunkManipulatorPanel::clearSelectionRequested,
                         mapView(), [this] { clearSelection(); });
        QObject::connect(_chunkManipulator, &ChunkManipulatorPanel::pasteRequested, mapView(), [this]
        {
            pasteAtCursor();
        });
        QObject::connect(_chunkManipulator, &ChunkManipulatorPanel::transformChanged, mapView(), [this]
        {
            if (_preview_suppressed)
            {
                _clipboard->clearPreview();
                return;
            }
            queuePreviewUpdate();
        });
        QObject::connect(_chunkManipulator, &ChunkManipulatorPanel::previewChanged, mapView(), [this]
        {
            if (_preview_suppressed)
            {
                _clipboard->clearPreview();
                return;
            }
            queuePreviewUpdate();
        });
        QObject::connect(_chunkManipulator, &ChunkManipulatorPanel::saveAssetRequested,
                         mapView(), [this](QString const& requested_name)
        {
            if (!_clipboard->hasCopy())
            {
                _chunkManipulator->showAssetStatus("Select and copy at least one chunk first.", true);
                return;
            }

            QString name = requested_name.trimmed();
            name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1F]"), "_");
            while (name.endsWith('.') || name.endsWith(' '))
                name.chop(1);
            if (name.isEmpty())
            {
                _chunkManipulator->showAssetStatus("The asset name is not valid.", true);
                return;
            }

            QDir directory(QString::fromStdString(
                Noggit::Project::CurrentProject::get()->ProjectPath) + "/noggit-assets/chunks");
            if (!directory.exists() && !directory.mkpath("."))
            {
                _chunkManipulator->showAssetStatus("Unable to create the project chunk library.", true);
                return;
            }
            QString const path = directory.filePath(name + ".nogchunk");
            if (QFileInfo::exists(path)
                && QMessageBox::question(_chunkManipulator, "Replace chunk asset",
                    QString("Replace the existing asset '%1'?").arg(name),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;

            _chunkManipulator->showAssetStatus(QString("Saving '%1'...").arg(name));
            _chunkManipulator->repaint();

            QString error;
            if (!_clipboard->saveAsset(path, &error))
            {
                _chunkManipulator->showAssetStatus(error, true);
                return;
            }
            _chunkManipulator->refreshAssetLibrary(path);
            _chunkManipulator->showAssetStatus(QString("Saved '%1'.").arg(name));
        });
        QObject::connect(_chunkManipulator, &ChunkManipulatorPanel::loadAssetRequested,
                         mapView(), [this](QString const& path)
        {
            QString error;
            if (!_clipboard->loadAsset(path, &error))
            {
                _chunkManipulator->showAssetStatus(error, true);
                return;
            }
            _selection_dirty = false;
            _chunkManipulator->refreshAssetLibrary(path);
            _chunkManipulator->showAssetStatus(
                QString("Loaded '%1' for placement.").arg(QFileInfo(path).completeBaseName()));
            queuePreviewUpdate();
        });
        QObject::connect(_chunkManipulator, &ChunkManipulatorPanel::deleteAssetRequested,
                         mapView(), [this](QString const& path)
        {
            QString const name = QFileInfo(path).completeBaseName();
            if (QMessageBox::question(_chunkManipulator, "Delete chunk asset",
                QString("Delete the saved asset '%1'? This cannot be undone.").arg(name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            if (!QFile::remove(path))
            {
                _chunkManipulator->showAssetStatus(QString("Unable to delete '%1'.").arg(name), true);
                return;
            }
            _chunkManipulator->refreshAssetLibrary();
            _chunkManipulator->showAssetStatus(QString("Deleted '%1'.").arg(name));
        });
    }

    ToolDrawParameters ChunkTool::drawParameters() const
    {
        return {.radius = brushRadius(), .cursor_type = CursorType::SQUARE,
                .cursor_color = {0.15f, 0.7f, 1.f, 1.f}};
    }

    float ChunkTool::brushRadius() const
    {
        return _chunkManipulator ? _chunkManipulator->brushRadius() : CHUNKSIZE;
    }

    void ChunkTool::clearSelection()
    {
        if (_clipboard)
            _clipboard->clearSelection();
        if (_chunkManipulator)
            _chunkManipulator->clearAssetSelection();
        _selection_dirty = false;
    }

    void ChunkTool::onSelected()
    {
        if (_clipboard)
            _clipboard->setOverlaysVisible(true);
        auto* params = mapView()->getWorld()->renderer()->getTerrainParamsUniformBlock();
        params->draw_selection_overlay = true;
        mapView()->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
    }

    void ChunkTool::onDeselected()
    {
        if (_clipboard)
            _clipboard->setOverlaysVisible(false);
        auto* params = mapView()->getWorld()->renderer()->getTerrainParamsUniformBlock();
        params->draw_selection_overlay = false;
        mapView()->getWorld()->renderer()->markTerrainParamsUniformBlockDirty();
    }

    void ChunkTool::onTick(float, TickParameters const& params)
    {
        if (!_clipboard)
            return;
        bool const was_preview_suppressed = _preview_suppressed;
        bool const was_adt_loading = _adt_loading;
        bool const adt_loading = updateAdtLoadingState();
        // Noggit3 hides the target and temporary chunk data while either
        // selection modifier is active.
        _preview_suppressed = params.mod_shift_down || params.mod_ctrl_down;
        if (was_preview_suppressed && !_preview_suppressed && _selection_dirty)
        {
            _selection_dirty = false;
            refreshClipboard();
        }
        if (_preview_suppressed)
        {
            _clipboard->clearPreview();
        }
        else if (_clipboard->hasCopy())
        {
            auto const options = _chunkManipulator->pasteOptions();
            auto const preview_options = _chunkManipulator->previewOptions();
            bool const target_changed = _clipboard->updatePreviewFootprint(
                mapView()->cursorPosition(), options, preview_options);
            if (!adt_loading && (target_changed || was_adt_loading))
                updatePreviewImmediately();
        }
        if (params.left_mouse && (params.mod_shift_down || params.mod_ctrl_down)
            && _clipboard->selectRange(mapView()->cursorPosition(), brushRadius(),
                true, params.mod_ctrl_down
                    ? ChunkSelectionMode::DESELECT : ChunkSelectionMode::SELECT))
            _selection_dirty = true;
    }

    void ChunkTool::onMousePress(MousePressParameters const& params)
    {
        if (!_clipboard || params.button != Qt::LeftButton || params.mod_alt_down || params.mod_space_down
            || (!params.mod_shift_down && !params.mod_ctrl_down))
            return;

        _preview_suppressed = params.mod_shift_down || params.mod_ctrl_down;
        if (_preview_suppressed)
            _clipboard->clearPreview();

        if (_clipboard->selectRange(mapView()->cursorPosition(), brushRadius(),
            true, params.mod_ctrl_down
                ? ChunkSelectionMode::DESELECT : ChunkSelectionMode::SELECT))
            _selection_dirty = true;
    }

    void ChunkTool::onMouseRelease(MouseReleaseParameters const& params)
    {
        if (params.button != Qt::LeftButton || !_selection_dirty || _preview_suppressed)
            return;

        _selection_dirty = false;
        refreshClipboard();
    }

    void ChunkTool::onMouseMove(MouseMoveParameters const& params)
    {
        if (_chunkManipulator && params.left_mouse && params.mod_alt_down
            && !params.mod_shift_down && !params.mod_ctrl_down)
            _chunkManipulator->changeRadius(params.relative_movement.dx() / XSENS);
    }

    void ChunkTool::refreshClipboard()
    {
        if (!_clipboard || !_chunkManipulator)
            return;
        if (_clipboard->copySelected())
        {
            if (_preview_suppressed)
            {
                _clipboard->clearPreview();
                return;
            }
            queuePreviewUpdate();
        }
    }

    void ChunkTool::queuePreviewUpdate()
    {
        if (!_clipboard || !_chunkManipulator || !_clipboard->hasCopy())
            return;
        bool const adt_loading = updateAdtLoadingState();
        auto const options = _chunkManipulator->pasteOptions();
        auto const preview_options = _chunkManipulator->previewOptions();
        _clipboard->updatePreviewFootprint(mapView()->cursorPosition(), options, preview_options);
        if (!adt_loading)
            updatePreviewImmediately();
    }

    void ChunkTool::updatePreviewImmediately()
    {
        if (!_clipboard || !_chunkManipulator || !_clipboard->hasCopy())
            return;
        if (updateAdtLoadingState())
            return;
        _clipboard->updatePreview(mapView()->cursorPosition(), _chunkManipulator->pasteOptions(),
                                  _chunkManipulator->previewOptions());
    }

    bool ChunkTool::updateAdtLoadingState()
    {
        bool const loading = mapView()->getWorld()->mapIndex.hasTilesAwaitingLoading();
        if (loading && !_adt_loading && _clipboard)
            _clipboard->clearPreview();
        _adt_loading = loading;
        return loading;
    }

    void ChunkTool::pasteAtCursor()
    {
        if (_selection_dirty)
        {
            _selection_dirty = false;
            refreshClipboard();
        }
        if (_clipboard && _chunkManipulator && _clipboard->hasCopy())
        {
            updatePreviewImmediately();
            _clipboard->pasteSelection(mapView()->cursorPosition(), _chunkManipulator->pasteOptions());
        }
    }

    void ChunkTool::onMouseWheel(MouseWheelParameters const& params)
    {
        if (!_chunkManipulator || !params.mod_space_down || params.mod_shift_down || params.mod_alt_down)
            return;
        float const precision = params.mod_ctrl_down ? 0.01f : 0.1f;
        float const change = precision * 10.f * params.event.angleDelta().y() / 320.f;
        _chunkManipulator->adjustHeightOffset(change);
    }

    void ChunkTool::unload()
    {
        if (_clipboard)
            _clipboard->setOverlaysVisible(false);
    }
}
