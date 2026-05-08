#include "ViewportManager.hpp"
#include <noggit/Log.h>
#include <noggit/TextureManager.h>

using namespace Noggit::Ui::Tools::ViewportManager;

std::vector<Viewport*> ViewportManager::_viewports;

Viewport::Viewport(QWidget* parent)
: QOpenGLWidget(parent)
{
  ViewportManager::registerViewport(this);
  _gl_connection = connect(this, &Viewport::aboutToLooseContext, [this]()
  {
    ViewportManager::unloadOpenglData(this);
  });

}

Noggit::NoggitRenderContext Noggit::Ui::Tools::ViewportManager::Viewport::getRenderContext() const
{
  return _context;
}

Viewport::~Viewport()
{
  ViewportManager::unregisterViewport(this);
}

void Noggit::Ui::Tools::ViewportManager::ViewportManager::registerViewport(Viewport* viewport)
{
  ViewportManager::_viewports.push_back(viewport);
}

void Noggit::Ui::Tools::ViewportManager::ViewportManager::unregisterViewport(Viewport* viewport)
{
  for (auto it = ViewportManager::_viewports.begin(); it != ViewportManager::_viewports.end(); ++it)
  {
    if (viewport == *it)
    {
      ViewportManager::_viewports.erase(it);
      break;
    }
  }
}

void ViewportManager::unloadOpenglData(Viewport* caller)
{
  for (auto viewport : ViewportManager::_viewports)
  {
    if (viewport == caller)
      continue;

    viewport->unloadOpenglData();
  }
}

void ViewportManager::unloadAll()
{
  for (auto viewport : ViewportManager::_viewports)
  {
    try
    {
      viewport->unloadOpenglData();
    }
    catch (std::exception const& e)
    {
      LogError << "A viewport could not release its OpenGL resources during cleanup. Widget type: "
        << viewport->metaObject()->className()
        << ". Error: " << e.what()
        << std::endl;
    }
    catch (...)
    {
      LogError << "A viewport could not release its OpenGL resources during cleanup. Widget type: "
        << viewport->metaObject()->className()
        << "."
        << std::endl;
    }
  }

  BLPRenderer::getInstance().unload();
}
