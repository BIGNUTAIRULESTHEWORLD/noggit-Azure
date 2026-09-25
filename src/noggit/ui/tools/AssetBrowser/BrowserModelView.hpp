#ifndef NOGGIT_ModelView_HPP
#define NOGGIT_ModelView_HPP

#include <QWidget>
#include <QPointF>
#include <QElapsedTimer>
#include <QTimer>
#include <QStringList>
#include <noggit/ui/tools/PreviewRenderer/PreviewRenderer.hpp>
#include <noggit/NpcAppearance.hpp>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

struct scoped_model_reference;
struct scoped_blp_texture_reference;
struct blp_texture;

class QWheelEvent;
class QMouseEvent;
class QFocusEvent;
class QKeyEvent;

namespace Noggit
{
  namespace Ui::Tools::AssetBrowser
  {

  class ModelViewer : public PreviewRenderer
    {
      Q_OBJECT

    public:
      explicit ModelViewer(QWidget* parent = nullptr
          , Noggit::NoggitRenderContext context = Noggit::NoggitRenderContext::ASSET_BROWSER
          , int offscreen_width = 0, int offscreen_height = 0);

      void setModel(std::string const& filename) override;
      bool setCreatureTexture(std::size_t type, std::string const& filename);
      void setCreatureGeosets(std::map<unsigned, unsigned> const& variants,
                              bool character_model, bool show_scalp = false);
      bool setCreatureAttachment(unsigned attachment_id, std::string const& model_path,
                                 std::string const& texture_path,
                                 unsigned render_attachment_id = 0);
      bool creatureAssetsPending() const { return _creature_assets_pending; }
      void clearCreatureAttachments();
      std::optional<Noggit::NpcAppearance> creatureAppearance() const;
      void setMoveSensitivity(float s);;
      float getMoveSensitivity() const;;
      QStringList getDoodadSetNames(std::string const& filename);
      void setActiveDoodadSet(std::string const& filename, std::string const& doodadset_name);
      std::string& getLastSelectedModel();;

      bool hasHeightForWidth() const override;;
      int heightForWidth(int w) const override;;

      ~ModelViewer() override;


    signals:
      void resized();
      void sensitivity_changed();
      void model_set(std::string const& filename);
      void gl_data_unloaded();

    protected:

      QTimer _update_every_event_loop;
      QPointF _last_mouse_pos;
      float moving, strafing, updown, mousedir, turn, lookat;
      bool look;
      std::string _last_selected_model;
      std::vector<unsigned> _npc_attachment_ids;
      std::vector<unsigned> _npc_attachment_render_ids;
      std::unordered_map<std::string, std::unique_ptr<scoped_model_reference>>
        _deferred_attachment_models;
      std::unordered_map<std::string, std::unique_ptr<scoped_blp_texture_reference>>
        _deferred_creature_textures;
      bool _creature_assets_pending = false;

      QElapsedTimer _startup_time;
      qreal _last_update = 0.f;
      bool _needs_redraw = false;
      float _move_sensitivity = 0.5f;

      QMetaObject::Connection _gl_guard_connection;

      void tick(float dt) override;
      std::optional<glm::mat4x4> modelInstanceTransform(std::size_t index) const override;
      float aspect_ratio() const override;

      void initializeGL() override;
      void paintGL() override;
      void resizeGL (int w, int h) override;

      void mouseMoveEvent(QMouseEvent* event) override;
      void mousePressEvent(QMouseEvent* event) override;
      void mouseReleaseEvent(QMouseEvent* event) override;
      void wheelEvent(QWheelEvent* event) override;
      void keyReleaseEvent(QKeyEvent* event) override;
      void keyPressEvent(QKeyEvent* event) override;
      void focusOutEvent(QFocusEvent* event) override;

    private:
        blp_texture* prefetchCreatureTexture(std::string const& path);
        std::array<Qt::Key, 6> _inputs = { Qt::Key_W, Qt::Key_S, Qt::Key_D, Qt::Key_A, Qt::Key_Q, Qt::Key_E };
        void checkInputsSettings();
    };
  }
}




#endif //NOGGIT_ModelView_HPP
