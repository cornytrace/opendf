
#include "engine.hpp"

#include <stdexcept>
#include <iostream>
#include <sstream>
#include <iomanip>

#include <SDL.h>
#include <SDL_syswm.h>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osg/MatrixTransform>
#include <osg/Depth>
#include <osg/CullFace>

#include "components/sdlutil/graphicswindow.hpp"
#include "components/vfs/manager.hpp"
#include "components/resource/texturemanager.hpp"
#include "components/dfosg/meshloader.hpp"

#include "gui/iface.hpp"
#include "input/input.hpp"
#include "log.hpp"


namespace DF
{

Engine::Engine(void)
  : mSDLWindow(nullptr)
{
}

Engine::~Engine(void)
{
    Log::get().setGuiIface(nullptr);
    GuiIface::get().deinitialize();

    mSceneRoot = nullptr;
    mCamera = nullptr;

    Input::get().deinitialize();

    if(mSDLWindow)
    {
        // If we don't do this, the desktop resolution is not restored on exit
        SDL_SetWindowFullscreen(mSDLWindow, 0);
        SDL_DestroyWindow(mSDLWindow);
        mSDLWindow = nullptr;
    }
    SDL_Quit();
}

bool Engine::parseOptions(int argc, char *argv[])
{
    for(int i = 1;i < argc;i++)
    {
        if(strcasecmp(argv[i], "-data") == 0)
        {
            if(i < argc-1)
                mRootPaths.push_back(argv[++i]);
        }
        else if(strcasecmp(argv[i], "-log") == 0)
        {
            if(i < argc-1)
                Log::get().setLog(argv[++i]);
        }
        else if(strcasecmp(argv[i], "-devparm") == 0)
            Log::get().setLevel(Log::Level_Debug);
        else
        {
            std::stringstream str;
            str<< "Unrecognized option: "<<argv[i];
            throw std::runtime_error(str.str());
        }
    }

    return true;
}


void Engine::handleWindowEvent(const SDL_WindowEvent &evt)
{
    switch(evt.event)
    {
        case SDL_WINDOWEVENT_MOVED:
        {
            int width, height;
            SDL_GetWindowSize(mSDLWindow, &width, &height);
            mCamera->getGraphicsContext()->resized(evt.data1, evt.data2, width, height);
            break;
        }
        case SDL_WINDOWEVENT_RESIZED:
        {
            int x, y;
            SDL_GetWindowPosition(mSDLWindow, &x, &y);
            mCamera->getGraphicsContext()->resized(x, y, evt.data1, evt.data2);
            break;
        }

        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_HIDDEN:
            break;

        case SDL_WINDOWEVENT_EXPOSED:
            // Needs redraw
            break;

        case SDL_WINDOWEVENT_ENTER:
        case SDL_WINDOWEVENT_LEAVE:
        case SDL_WINDOWEVENT_FOCUS_GAINED:
        case SDL_WINDOWEVENT_FOCUS_LOST:
            break;

        case SDL_WINDOWEVENT_CLOSE:
            // FIXME: Inject an SDL_QUIT event? Seems to happen anyway...
            break;

        default:
            std::cerr<< "Unhandled window event: "<<(int)evt.event <<std::endl;
    }
}

bool Engine::pumpEvents()
{
    SDL_PumpEvents();

    SDL_Event evt;
    while(SDL_PollEvent(&evt))
    {
        switch(evt.type)
        {
        case SDL_WINDOWEVENT:
            handleWindowEvent(evt.window);
            break;

        case SDL_MOUSEMOTION:
            Input::get().handleMouseMotionEvent(evt.motion);

            if(GuiIface::get().getMode() == GuiIface::Mode_Game)
            {
                /* HACK: mouse rotates the camera around */
                static float x=0.0f, y=0.0f;
                /* Rotation (x motion rotates around y, y motion rotates around x) */
                x += evt.motion.yrel * 0.1f;
                y += evt.motion.xrel * 0.1f;
                x = std::min(std::max(x, -89.0f), 89.0f);

                mCameraRot.makeRotate(
                     x*3.14159f/180.0f, osg::Vec3f(1.0f, 0.0f, 0.0f),
                    -y*3.14159f/180.0f, osg::Vec3f(0.0f, 1.0f, 0.0f),
                                  0.0f, osg::Vec3f(0.0f, 0.0f, 1.0f)
                );
            }
            break;
        case SDL_MOUSEWHEEL:
            Input::get().handleMouseWheelEvent(evt.wheel);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            Input::get().handleMouseButtonEvent(evt.button);
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            Input::get().handleKeyboardEvent(evt.key);
            break;
        case SDL_TEXTINPUT:
            Input::get().handleTextInputEvent(evt.text);
            break;

        case SDL_QUIT:
            return false;
        }
    }

    return true;
}


bool Engine::go(void)
{
    Log::get().initialize();

    // Init everything except audio (we will use OpenAL for that)
    Log::get().message("Initializing SDL...");
    if(SDL_Init(SDL_INIT_EVERYTHING & ~SDL_INIT_AUDIO) != 0)
    {
        std::stringstream sstr;
        sstr<< "SDL_Init Error: "<<SDL_GetError();
        throw std::runtime_error(sstr.str());
    }

    if(mRootPaths.empty())
    {
        Log::get().message("Initializing VFS...");
        VFS::Manager::get().initialize();
    }
    else
    {
        Log::get().stream()<< "Initializing VFS with root "<<mRootPaths.front()<<"...";
        VFS::Manager::get().initialize(mRootPaths.front());
        auto path = mRootPaths.begin()+1;
        for(;path != mRootPaths.end();++path)
        {
            Log::get().stream()<< "Adding data path "<<*path<<"...";
            VFS::Manager::get().addDataPath(*path);
        }
    }

    // Configure
    osg::ref_ptr<osgViewer::Viewer> viewer;
    {
        int width = 1024;
        int height = 768;
        int xpos = SDL_WINDOWPOS_CENTERED;
        int ypos = SDL_WINDOWPOS_CENTERED;
        Uint32 flags = SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN;

        SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, SDL_TRUE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, SDL_TRUE);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

        Log::get().stream()<< "Creating window "<<width<<"x"<<height<<", flags 0x"<<std::hex<<flags;
        mSDLWindow = SDL_CreateWindow("OpenDF", xpos, ypos, width, height, flags);
        if(mSDLWindow == nullptr)
        {
            std::stringstream sstr;
            sstr<< "SDL_CreateWindow Error: "<<SDL_GetError();
            throw std::runtime_error(sstr.str());
        }

        SDLUtil::graphicswindow_SDL2();
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        SDL_GetWindowPosition(mSDLWindow, &traits->x, &traits->y);
        SDL_GetWindowSize(mSDLWindow, &traits->width, &traits->height);
        traits->windowName = SDL_GetWindowTitle(mSDLWindow);
        traits->windowDecoration = !(SDL_GetWindowFlags(mSDLWindow)&SDL_WINDOW_BORDERLESS);
        traits->screenNum = SDL_GetWindowDisplayIndex(mSDLWindow);
        // FIXME: Some way to get these settings back from the SDL window?
        traits->red = 8;
        traits->green = 8;
        traits->blue = 8;
        traits->alpha = 0;
        traits->depth = 24;
        traits->stencil = 8;
        traits->doubleBuffer = true;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mSDLWindow);

        osg::ref_ptr<osg::GraphicsContext> gc = osg::GraphicsContext::createGraphicsContext(traits.get());
        if(!gc.valid()) throw std::runtime_error("Failed to create GraphicsContext");
        //gc->getState()->setUseModelViewAndProjectionUniforms(true);
        //gc->getState()->setUseVertexAttributeAliasing(true);

        mCamera = new osg::Camera();
        mCamera->setGraphicsContext(gc.get());
        mCamera->setViewport(0, 0, width, height);
        mCamera->setProjectionResizePolicy(osg::Camera::FIXED);
        mCamera->setProjectionMatrix(osg::Matrix::perspective(65.0, double(width)/double(height), 1.0, 10000.0));
        mCamera->setClearColor(osg::Vec4());
        mCamera->setClearDepth(1.0);

        viewer = new osgViewer::Viewer();
        viewer->setCamera(mCamera.get());
    }
    SDL_ShowCursor(0);

    Log::get().message("Initializing Texture Manager...");
    Resource::TextureManager::get().initialize();

    Log::get().message("Initializing Input...");
    Input::get().initialize();

    {
        mSceneRoot = new osg::MatrixTransform(osg::Matrix::scale(osg::Vec3(1.0f, -1.0f, -1.0f)));
        osg::StateSet *ss = mSceneRoot->getOrCreateStateSet();
        ss->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0.0, 1.0, true));
        ss->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK));
        ss->setMode(GL_BLEND, osg::StateAttribute::OFF);
    }

    viewer->setSceneData(mSceneRoot);
    viewer->requestContinuousUpdate();
    {
        viewer->setLightingMode(osg::View::HEADLIGHT);
        osg::ref_ptr<osg::Light> light(new osg::Light());
        light->setAmbient(osg::Vec4(0.5f, 0.5f, 0.5f, 1.0f));
        viewer->setLight(light);
    }
    viewer->addEventHandler(new osgViewer::StatsHandler);
    viewer->realize();

    Log::get().message("Initializing GUI...");
    GuiIface::get().initialize(viewer, mSceneRoot.get());
    Log::get().setGuiIface(&GuiIface::get());

    {
        osg::ref_ptr<osg::Node> node = DFOSG::MeshLoader::get().load(0);
        mSceneRoot->addChild(node);
    }

    mCameraPos.z() = 64.0;

    // And away we go!
    Uint32 last_tick = SDL_GetTicks();
    while(!viewer->done() && pumpEvents())
    {
        const Uint8 *keystate = SDL_GetKeyboardState(NULL);
        if(keystate[SDL_SCANCODE_ESCAPE])
            break;

        Uint32 current_tick = SDL_GetTicks();
        Uint32 tick_count = current_tick - last_tick;
        last_tick = current_tick;
        float timediff = tick_count / 1000.0;

        if(GuiIface::get().getMode() == GuiIface::Mode_Game)
        {
            float speed = 16.0f * timediff;
            if(keystate[SDL_SCANCODE_LSHIFT])
                speed *= 2.0f;

            osg::Vec3f movedir;
            if(keystate[SDL_SCANCODE_W])
                movedir.z() += -1.0f;
            if(keystate[SDL_SCANCODE_A])
                movedir.x() += -1.0f;
            if(keystate[SDL_SCANCODE_S])
                movedir.z() += +1.0f;
            if(keystate[SDL_SCANCODE_D])
                movedir.x() += +1.0f;
            if(keystate[SDL_SCANCODE_PAGEUP])
                movedir.y() += +1.0f;
            if(keystate[SDL_SCANCODE_PAGEDOWN])
                movedir.y() += -1.0f;


            mCameraPos += (mCameraRot*movedir)*speed;

            osg::Matrixf matf(mCameraRot.inverse());
            matf.preMultTranslate(-mCameraPos);
            mCamera->setViewMatrix(matf);
        }

        viewer->frame(timediff);
    }
    Log::get().message("Main loop shutting down...");

    return true;
}

} // namespace DF