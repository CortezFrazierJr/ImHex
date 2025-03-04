#include "window.hpp"

#include <hex.hpp>

#include <hex/api/plugin_manager.hpp>
#include <hex/api/content_registry.hpp>
#include <hex/api/imhex_api.hpp>

#include <hex/helpers/utils.hpp>
#include <hex/helpers/fs.hpp>
#include <hex/helpers/logger.hpp>
#include <fmt/printf.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <set>
#include <thread>
#include <cassert>

#include <romfs/romfs.hpp>

#include <imgui.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <hex/ui/imgui_imhex_extensions.h>
#include <implot.h>
#include <implot_internal.h>
#include <imnodes.h>
#include <imnodes_internal.h>

#include <fonts/codicons_font.h>

#include <hex/api/project_file_manager.hpp>

#include <GLFW/glfw3.h>

namespace hex {

    using namespace std::literals::chrono_literals;

    void *ImHexSettingsHandler_ReadOpenFn(ImGuiContext *ctx, ImGuiSettingsHandler *, const char *) {
        return ctx;    // Unused, but the return value has to be non-null
    }

    void ImHexSettingsHandler_ReadLine(ImGuiContext *, ImGuiSettingsHandler *, void *, const char *line) {
        for (auto &[name, view] : ContentRegistry::Views::getEntries()) {
            std::string format = view->getUnlocalizedName() + "=%d";
            sscanf(line, format.c_str(), &view->getWindowOpenState());
        }
        for (auto &[name, function, detached] : ContentRegistry::Tools::getEntries()) {
            std::string format = name + "=%d";
            sscanf(line, format.c_str(), &detached);
        }
    }

    void ImHexSettingsHandler_WriteAll(ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf) {
        buf->appendf("[%s][General]\n", handler->TypeName);

        for (auto &[name, view] : ContentRegistry::Views::getEntries()) {
            buf->appendf("%s=%d\n", name.c_str(), view->getWindowOpenState());
        }
        for (auto &[name, function, detached] : ContentRegistry::Tools::getEntries()) {
            buf->appendf("%s=%d\n", name.c_str(), detached);
        }

        buf->append("\n");
    }

    static void signalHandler(int signalNumber, std::string signalName) {
        log::fatal("Terminating with signal '{}' ({})", signalName, signalNumber);

        EventManager::post<EventAbnormalTermination>(signalNumber);

        if (std::uncaught_exceptions() > 0) {
            log::fatal("Uncaught exception thrown!");
        }

        // Let's not loop on this...
        std::signal(signalNumber, nullptr);

        #if defined(DEBUG)
            assert(false);
        #else
            std::raise(signalNumber);
        #endif
    }

    Window::Window() {
        {
            for (const auto &[argument, value] : ImHexApi::System::getInitArguments()) {
                if (argument == "no-plugins") {
                    TaskManager::doLater([] { ImGui::OpenPopup("No Plugins"); });
                } else if (argument == "no-builtin-plugin") {
                    TaskManager::doLater([] { ImGui::OpenPopup("No Builtin Plugin"); });
                } else if (argument == "multiple-builtin-plugins") {
                    TaskManager::doLater([] { ImGui::OpenPopup("Multiple Builtin Plugins"); });
                }
            }
        }

        this->initGLFW();
        this->initImGui();
        this->setupNativeWindow();

        // Initialize default theme
        EventManager::post<RequestChangeTheme>(1);

        EventManager::subscribe<RequestCloseImHex>(this, [this](bool noQuestions) {
            glfwSetWindowShouldClose(this->m_window, GLFW_TRUE);

            if (!noQuestions)
                EventManager::post<EventWindowClosing>(this->m_window);
        });

        EventManager::subscribe<RequestChangeWindowTitle>(this, [this](const std::string &windowTitle) {
            std::string title = "ImHex";

            if (ImHexApi::Provider::isValid()) {
                auto provider = ImHexApi::Provider::get();
                if (!windowTitle.empty() && provider != nullptr) {
                    title += " - " + windowTitle;

                    if (provider->isDirty())
                        title += " (*)";

                    if (!provider->isWritable())
                        title += " (Read Only)";
                }
            }

            this->m_windowTitle = title;
            glfwSetWindowTitle(this->m_window, title.c_str());
        });

        constexpr static auto CrashBackupFileName = "crash_backup.hexproj";

        EventManager::subscribe<EventAbnormalTermination>(this, [this](int) {
            ImGui::SaveIniSettingsToDisk(hex::toUTF8String(this->m_imguiSettingsPath).c_str());

            if (!ImHexApi::Provider::isDirty())
                return;

            for (const auto &path : fs::getDefaultPaths(fs::ImHexPath::Config)) {
                if (ProjectFile::store(path / CrashBackupFileName))
                    break;
            }
        });

        EventManager::subscribe<RequestOpenPopup>(this, [this](auto name) {
            std::scoped_lock lock(this->m_popupMutex);

            this->m_popupsToOpen.push_back(name);
        });

        #define HANDLE_SIGNAL(name) \
		std::signal(name, [](int signalNumber){ \
            signalHandler(signalNumber, #name); \
        });
        HANDLE_SIGNAL(SIGSEGV)
        HANDLE_SIGNAL(SIGILL)
        HANDLE_SIGNAL(SIGABRT)
        HANDLE_SIGNAL(SIGFPE)
        #undef HANDLE_SIGNAL
        std::set_terminate([]{ signalHandler(SIGABRT, "Unhandled C++ exception"); });

        auto logoData      = romfs::get("logo.png");
        this->m_logoTexture = ImGui::Texture(reinterpret_cast<const ImU8 *>(logoData.data()), logoData.size());

        ContentRegistry::Settings::store();
        EventManager::post<EventSettingsChanged>();
        EventManager::post<EventWindowInitialized>();
    }

    Window::~Window() {
        EventManager::unsubscribe<EventProviderDeleted>(this);
        EventManager::unsubscribe<RequestCloseImHex>(this);
        EventManager::unsubscribe<RequestChangeWindowTitle>(this);
        EventManager::unsubscribe<EventAbnormalTermination>(this);
        EventManager::unsubscribe<RequestOpenPopup>(this);

        this->exitImGui();
        this->exitGLFW();
    }

    void Window::loop() {
        this->m_lastFrameTime = glfwGetTime();
        while (!glfwWindowShouldClose(this->m_window)) {
            if (!glfwGetWindowAttrib(this->m_window, GLFW_VISIBLE) || glfwGetWindowAttrib(this->m_window, GLFW_ICONIFIED)) {
                glfwWaitEvents();
            } else {
                glfwPollEvents();

                bool frameRateUnlocked = ImGui::IsPopupOpen(ImGuiID(0), ImGuiPopupFlags_AnyPopupId) || TaskManager::getRunningTaskCount() > 0 || this->m_mouseButtonDown || this->m_hadEvent || !this->m_pressedKeys.empty();
                const double timeout = std::max(0.0, (1.0 / 5.0) - (glfwGetTime() - this->m_lastFrameTime));

                if ((this->m_lastFrameTime - this->m_frameRateUnlockTime) > 5 && this->m_frameRateTemporarilyUnlocked && !frameRateUnlocked) {
                    this->m_frameRateTemporarilyUnlocked = false;
                }

                if (frameRateUnlocked || this->m_frameRateTemporarilyUnlocked) {
                    if (!this->m_frameRateTemporarilyUnlocked) {
                        this->m_frameRateTemporarilyUnlocked = true;
                        this->m_frameRateUnlockTime = this->m_lastFrameTime;
                    }
                } else {
                    glfwWaitEventsTimeout(timeout);
                }
            }

            this->frameBegin();
            this->frame();
            this->frameEnd();

            const auto targetFps = ImHexApi::System::getTargetFPS();
            if (targetFps <= 200) {
                auto leftoverFrameTime = i64((this->m_lastFrameTime + 1 / targetFps - glfwGetTime()) * 1000);
                if (leftoverFrameTime > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(leftoverFrameTime));
            }

            this->m_lastFrameTime = glfwGetTime();

            this->m_hadEvent = false;
        }
    }

    void Window::frameBegin() {

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImHexApi::System::getMainWindowSize() - ImVec2(0, ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (ImGui::Begin("ImHexDockSpace", nullptr, windowFlags)) {
            auto drawList = ImGui::GetWindowDrawList();
            ImGui::PopStyleVar();
            auto sidebarPos   = ImGui::GetCursorPos();
            auto sidebarWidth = ContentRegistry::Interface::getSidebarItems().empty() ? 0 : 30_scaled;

            ImGui::SetCursorPosX(sidebarWidth);

            auto footerHeight  = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 2 + 1_scaled;
            auto dockSpaceSize = ImVec2(ImHexApi::System::getMainWindowSize().x - sidebarWidth, ImGui::GetContentRegionAvail().y - footerHeight);

            auto dockId = ImGui::DockSpace(ImGui::GetID("ImHexMainDock"), dockSpaceSize);
            ImHexApi::System::impl::setMainDockSpaceId(dockId);

            drawList->AddRectFilled(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImGui::GetWindowSize() - ImVec2(dockSpaceSize.x, footerHeight - ImGui::GetStyle().FramePadding.y - 1_scaled), ImGui::GetColorU32(ImGuiCol_MenuBarBg));

            ImGui::Separator();
            ImGui::SetCursorPosX(8);
            for (const auto &callback : ContentRegistry::Interface::getFooterItems()) {
                auto prevIdx = drawList->_VtxCurrentIdx;
                callback();
                auto currIdx = drawList->_VtxCurrentIdx;

                // Only draw separator if something was actually drawn
                if (prevIdx != currIdx) {
                    ImGui::SameLine();
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                }
            }

            {
                ImGui::SetCursorPos(sidebarPos);

                static i32 openWindow = -1;
                u32 index             = 0;
                ImGui::PushID("SideBarWindows");
                for (const auto &[icon, callback] : ContentRegistry::Interface::getSidebarItems()) {
                    ImGui::SetCursorPosY(sidebarPos.y + sidebarWidth * index);

                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetColorU32(ImGuiCol_ScrollbarGrabActive));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetColorU32(ImGuiCol_ScrollbarGrabHovered));

                    ImGui::BeginDisabled(!ImHexApi::Provider::isValid());
                    {
                        if (ImGui::Button(icon.c_str(), ImVec2(sidebarWidth, sidebarWidth))) {
                            if (static_cast<u32>(openWindow) == index)
                                openWindow = -1;
                            else
                                openWindow = index;
                        }
                    }
                    ImGui::EndDisabled();

                    ImGui::PopStyleColor(3);

                    bool open = static_cast<u32>(openWindow) == index;
                    if (open) {
                        ImGui::SetNextWindowPos(ImGui::GetWindowPos() + sidebarPos + ImVec2(sidebarWidth - 2_scaled, 0));
                        ImGui::SetNextWindowSize(ImVec2(250_scaled, dockSpaceSize.y + ImGui::GetStyle().FramePadding.y + 2_scaled));

                        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
                        if (ImGui::Begin("Window", &open, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
                            callback();
                        }
                        ImGui::End();
                        ImGui::PopStyleVar();
                    }

                    ImGui::NewLine();
                    index++;
                }
                ImGui::PopID();
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::BeginMainMenuBar()) {

                if (ImHexApi::System::isBorderlessWindowModeEnabled()) {
                    auto menuBarHeight = ImGui::GetCurrentWindow()->MenuBarHeight();
                    ImGui::SetCursorPosX(5);
                    ImGui::Image(this->m_logoTexture, ImVec2(menuBarHeight, menuBarHeight));
                }

                for (const auto &[priority, menuItem] : ContentRegistry::Interface::getMainMenuItems()) {
                    if (ImGui::BeginMenu(LangEntry(menuItem.unlocalizedName))) {
                        ImGui::EndMenu();
                    }
                }

                std::set<std::string> encounteredMenus;
                for (auto &[priority, menuItem] : ContentRegistry::Interface::getMenuItems()) {
                    if (ImGui::BeginMenu(LangEntry(menuItem.unlocalizedName))) {
                        auto [iter, inserted] = encounteredMenus.insert(menuItem.unlocalizedName);
                        if (!inserted)
                            ImGui::Separator();

                        menuItem.callback();
                        ImGui::EndMenu();
                    }
                }

                this->drawTitleBar();

                ImGui::EndMainMenuBar();
            }
            ImGui::PopStyleVar();

            // Draw toolbar
            if (ImGui::BeginMenuBar()) {

                for (const auto &callback : ContentRegistry::Interface::getToolbarItems()) {
                    callback();
                    ImGui::SameLine();
                }

                ImGui::EndMenuBar();
            }

            this->beginNativeWindowFrame();

            drawList->AddLine(ImGui::GetWindowPos() + ImVec2(sidebarWidth - 2, 0), ImGui::GetWindowPos() + ImGui::GetWindowSize() - ImVec2(dockSpaceSize.x + 2, footerHeight - ImGui::GetStyle().FramePadding.y - 2), ImGui::GetColorU32(ImGuiCol_Separator));
            drawList->AddLine(ImGui::GetWindowPos() + ImVec2(sidebarWidth, ImGui::GetCurrentWindow()->MenuBarHeight()), ImGui::GetWindowPos() + ImVec2(ImGui::GetWindowSize().x, ImGui::GetCurrentWindow()->MenuBarHeight()), ImGui::GetColorU32(ImGuiCol_Separator));
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        // Plugin load error popups. These are not translated because they should always be readable, no matter if any localization could be loaded or not
        {
            auto drawPluginFolderTable = []() {
                ImGui::UnderlinedText("Plugin folders");
                if (ImGui::BeginTable("plugins", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.2);
                    ImGui::TableSetupColumn("Exists", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() * 3);

                    ImGui::TableHeadersRow();

                    for (const auto &path : fs::getDefaultPaths(fs::ImHexPath::Plugins, true)) {
                        const auto filePath = path / "builtin.hexplug";
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(hex::toUTF8String(filePath).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(fs::exists(filePath) ? ICON_VS_CHECK : ICON_VS_CLOSE);
                    }
                    ImGui::EndTable();
                }
            };

            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
            if (ImGui::BeginPopupModal("No Plugins", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
                ImGui::TextUnformatted("No ImHex plugins loaded (including the built-in plugin)!");
                ImGui::TextUnformatted("Make sure you installed ImHex correctly.");
                ImGui::TextUnformatted("There should be at least a 'builtin.hexplug' file in your plugins folder.");

                ImGui::NewLine();

                drawPluginFolderTable();

                ImGui::EndPopup();
            }

            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
            if (ImGui::BeginPopupModal("No Builtin Plugin", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
                ImGui::TextUnformatted("The ImHex built-in plugins could not be loaded!");
                ImGui::TextUnformatted("Make sure you installed ImHex correctly.");
                ImGui::TextUnformatted("There should be at least a 'builtin.hexplug' file in your plugins folder.");

                ImGui::NewLine();

                drawPluginFolderTable();

                ImGui::EndPopup();
            }

            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
            if (ImGui::BeginPopupModal("Multiple Builtin Plugins", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
                ImGui::TextUnformatted("ImHex found and attempted to load multiple built-in plugins!");
                ImGui::TextUnformatted("Make sure you installed ImHex correctly and, if needed,");
                ImGui::TextUnformatted("cleaned up older installations correctly,");
                ImGui::TextUnformatted("There should be exactly one 'builtin.hexplug' file in any one your plugin folders.");

                ImGui::NewLine();

                drawPluginFolderTable();

                ImGui::EndPopup();
            }
        }
        {
            std::scoped_lock lock(this->m_popupMutex);
            this->m_popupsToOpen.remove_if([](const auto &name) {
                if (ImGui::IsPopupOpen(name.c_str()))
                    return true;
                else
                    ImGui::OpenPopup(name.c_str());

                return false;
            });
        }

        TaskManager::runDeferredCalls();

        EventManager::post<EventFrameBegin>();
    }

    void Window::frame() {
        auto &io = ImGui::GetIO();
        for (auto &[name, view] : ContentRegistry::Views::getEntries()) {
            ImGui::GetCurrentContext()->NextWindowData.ClearFlags();

            view->drawAlwaysVisible();

            if (!view->shouldProcess())
                continue;

            if (view->isAvailable()) {
                float fontScaling = std::max(1.0F, ImHexApi::System::getFontSize() / ImHexApi::System::DefaultFontSize);
                ImGui::SetNextWindowSizeConstraints(view->getMinSize() * fontScaling, view->getMaxSize() * fontScaling);
                view->drawContent();
            }

            if (view->getWindowOpenState()) {
                auto window    = ImGui::FindWindowByName(view->getName().c_str());
                bool hasWindow = window != nullptr;
                bool focused   = false;


                if (hasWindow && !(window->Flags & ImGuiWindowFlags_Popup)) {
                    ImGui::Begin(View::toWindowName(name).c_str());

                    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_NoPopupHierarchy);
                    ImGui::End();
                }

                for (const auto &key : this->m_pressedKeys) {
                    ShortcutManager::process(view, io.KeyCtrl, io.KeyAlt, io.KeyShift, io.KeySuper, focused, key);
                }
            }
        }

        for (const auto &key : this->m_pressedKeys) {
            ShortcutManager::processGlobals(io.KeyCtrl, io.KeyAlt, io.KeyShift, io.KeySuper, key);
        }

        this->m_pressedKeys.clear();
    }

    void Window::frameEnd() {
        EventManager::post<EventFrameEnd>();

        TaskManager::collectGarbage();

        this->endNativeWindowFrame();
        ImGui::Render();

        int displayWidth, displayHeight;
        glfwGetFramebufferSize(this->m_window, &displayWidth, &displayHeight);
        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.00F, 0.00F, 0.00F, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        GLFWwindow *backup_current_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup_current_context);

        glfwSwapBuffers(this->m_window);
    }

    void Window::initGLFW() {
        glfwSetErrorCallback([](int error, const char *desc) {
            log::error("GLFW Error [{}] : {}", error, desc);
        });

        if (!glfwInit()) {
            log::fatal("Failed to initialize GLFW!");
            std::abort();
        }

        #if defined(OS_MACOS)
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        #else
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        #endif

        glfwWindowHint(GLFW_DECORATED, ImHexApi::System::isBorderlessWindowModeEnabled() ? GL_FALSE : GL_TRUE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        this->m_windowTitle = "ImHex";
        this->m_window      = glfwCreateWindow(1280_scaled, 720_scaled, this->m_windowTitle.c_str(), nullptr, nullptr);

        glfwSetWindowUserPointer(this->m_window, this);

        if (this->m_window == nullptr) {
            log::fatal("Failed to create window!");
            std::abort();
        }

        glfwMakeContextCurrent(this->m_window);
        glfwSwapInterval(1);

        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (monitor != nullptr) {
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            if (mode != nullptr) {
                int monitorX, monitorY;
                glfwGetMonitorPos(monitor, &monitorX, &monitorY);

                int windowWidth, windowHeight;
                glfwGetWindowSize(this->m_window, &windowWidth, &windowHeight);

                glfwSetWindowPos(this->m_window, monitorX + (mode->width - windowWidth) / 2, monitorY + (mode->height - windowHeight) / 2);
            }
        }

        {
            int x = 0, y = 0;
            glfwGetWindowPos(this->m_window, &x, &y);

            ImHexApi::System::impl::setMainWindowPosition(x, y);
        }

        {
            int width = 0, height = 0;
            glfwGetWindowSize(this->m_window, &width, &height);
            glfwSetWindowSize(this->m_window, width, height);
            ImHexApi::System::impl::setMainWindowSize(width, height);
        }

        glfwSetWindowPosCallback(this->m_window, [](GLFWwindow *window, int x, int y) {
            ImHexApi::System::impl::setMainWindowPosition(x, y);

            if (auto g = ImGui::GetCurrentContext(); g == nullptr || g->WithinFrameScope) return;

            auto win = static_cast<Window *>(glfwGetWindowUserPointer(window));
            win->frameBegin();
            win->frame();
            win->frameEnd();
            win->processEvent();
        });

        glfwSetWindowSizeCallback(this->m_window, [](GLFWwindow *window, int width, int height) {
            if (!glfwGetWindowAttrib(window, GLFW_ICONIFIED))
                ImHexApi::System::impl::setMainWindowSize(width, height);

            if (auto g = ImGui::GetCurrentContext(); g == nullptr || g->WithinFrameScope) return;

            auto win = static_cast<Window *>(glfwGetWindowUserPointer(window));
            win->frameBegin();
            win->frame();
            win->frameEnd();
            win->processEvent();
        });

        glfwSetMouseButtonCallback(this->m_window, [](GLFWwindow *window, int button, int action, int mods) {
            hex::unused(button, mods);

            auto win = static_cast<Window *>(glfwGetWindowUserPointer(window));

            if (action == GLFW_PRESS)
                win->m_mouseButtonDown = true;
            else if (action == GLFW_RELEASE)
                win->m_mouseButtonDown = false;
            win->processEvent();
        });

        glfwSetKeyCallback(this->m_window, [](GLFWwindow *window, int key, int scancode, int action, int mods) {
            hex::unused(mods);

            auto keyName = glfwGetKeyName(key, scancode);
            if (keyName != nullptr)
                key = std::toupper(keyName[0]);

            auto win = static_cast<Window *>(glfwGetWindowUserPointer(window));

            if (action == GLFW_PRESS || action == GLFW_REPEAT) {
                win->m_pressedKeys.push_back(key);
            }
            win->processEvent();
        });

        glfwSetDropCallback(this->m_window, [](GLFWwindow *, int count, const char **paths) {
            if (count != 1)
                return;

            for (int i = 0; i < count; i++) {
                auto path = std::fs::path(reinterpret_cast<const char8_t *>(paths[i]));

                bool handled = false;
                for (const auto &[extensions, handler] : ContentRegistry::FileHandler::getEntries()) {
                    for (const auto &extension : extensions) {
                        if (path.extension() == extension) {
                            if (!handler(path))
                                log::error("Handler for extensions '{}' failed to process file!", extension);

                            handled = true;
                            break;
                        }
                    }
                }

                if (!handled)
                    EventManager::post<RequestOpenFile>(path);
            }
        });

        glfwSetCursorPosCallback(this->m_window, [](GLFWwindow *window, double x, double y) {
            hex::unused(x, y);

            auto win = static_cast<Window *>(glfwGetWindowUserPointer(window));
            win->processEvent();
        });

        glfwSetWindowCloseCallback(this->m_window, [](GLFWwindow *window) {
            EventManager::post<EventWindowClosing>(window);
        });

        glfwSetWindowSizeLimits(this->m_window, 720_scaled, 480_scaled, GLFW_DONT_CARE, GLFW_DONT_CARE);

        glfwShowWindow(this->m_window);
    }

    void Window::initImGui() {
        IMGUI_CHECKVERSION();

        auto fonts = View::getFontAtlas();

        GImGui   = ImGui::CreateContext(fonts);
        GImPlot  = ImPlot::CreateContext();
        GImNodes = ImNodes::CreateContext();

        ImGuiIO &io       = ImGui::GetIO();
        ImGuiStyle &style = ImGui::GetStyle();

        style.Alpha          = 1.0F;
        style.WindowRounding = 0.0F;

        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;

        if (glfwGetPrimaryMonitor() != nullptr) {
            auto sessionType = hex::getEnvironmentVariable("XDG_SESSION_TYPE");
            bool multiWindowEnabled = ContentRegistry::Settings::read("hex.builtin.setting.interface", "hex.builtin.setting.interface.multi_windows", 1) != 0;

            if ((!sessionType || !hex::containsIgnoreCase(*sessionType, "wayland")) && multiWindowEnabled)
                io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        }

        for (auto &entry : fonts->ConfigData)
            io.Fonts->ConfigData.push_back(entry);

        io.ConfigViewportsNoTaskBarIcon = false;

        ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkDetachWithDragClick);
        ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkCreationOnSnap);

        {
            static bool always                                    = true;
            ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = &always;
        }

        io.UserData = new ImGui::ImHexCustomData();

        auto scale = ImHexApi::System::getGlobalScale();
        style.ScaleAllSizes(scale);
        io.DisplayFramebufferScale = ImVec2(scale, scale);

        {
            GLsizei width, height;
            u8 *fontData;

            io.Fonts->GetTexDataAsRGBA32(&fontData, &width, &height);

            // Create new font atlas
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA8, GL_UNSIGNED_INT, fontData);
            io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(tex));
        }

        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.IndentSpacing            = 10.0F;

        // Install custom settings handler
        ImGuiSettingsHandler handler;
        handler.TypeName   = "ImHex";
        handler.TypeHash   = ImHashStr("ImHex");
        handler.ReadOpenFn = ImHexSettingsHandler_ReadOpenFn;
        handler.ReadLineFn = ImHexSettingsHandler_ReadLine;
        handler.WriteAllFn = ImHexSettingsHandler_WriteAll;
        handler.UserData   = this;
        ImGui::GetCurrentContext()->SettingsHandlers.push_back(handler);

        for (const auto &dir : fs::getDefaultPaths(fs::ImHexPath::Config)) {
            if (std::fs::exists(dir) && fs::isPathWritable(dir)) {
                this->m_imguiSettingsPath = dir / "interface.ini";
                io.IniFilename            = nullptr;
                break;
            }
        }

        if (!this->m_imguiSettingsPath.empty() && fs::exists(this->m_imguiSettingsPath))
            ImGui::LoadIniSettingsFromDisk(hex::toUTF8String(this->m_imguiSettingsPath).c_str());

        ImGui_ImplGlfw_InitForOpenGL(this->m_window, true);

        #if defined(OS_MACOS)
            ImGui_ImplOpenGL3_Init("#version 150");
        #else
            ImGui_ImplOpenGL3_Init("#version 130");
        #endif

        for (const auto &plugin : PluginManager::getPlugins())
            plugin.setImGuiContext(ImGui::GetCurrentContext());
    }

    void Window::exitGLFW() {
        glfwDestroyWindow(this->m_window);
        glfwTerminate();
    }

    void Window::exitImGui() {
        delete static_cast<ImGui::ImHexCustomData *>(ImGui::GetIO().UserData);

        ImNodes::PopAttributeFlag();
        ImNodes::PopAttributeFlag();

        ImGui::SaveIniSettingsToDisk(hex::toUTF8String(this->m_imguiSettingsPath).c_str());

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImNodes::DestroyContext();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

}
